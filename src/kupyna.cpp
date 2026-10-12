#include "kupyna.hpp"
#include "kupyna_tables.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

// Формулы ДСТУ 7564:2014 и таблицы сверены с предоставленной в задании
// Kupyna-reference. Авторы эталона: Ruslan Kiianchuk, Ruslan Mordvinov,
// Roman Oliynykov. Здесь явный порядок байтов и пошаговый callback,
// чтобы результат не зависел от архитектуры и выравнивания указателей.
namespace kupyna {
namespace {

constexpr std::size_t ROWS = 8;
constexpr std::size_t LENGTH_BYTES = 12; // В дополнении записываются 96 бит длины.

class Reporter {
public:
    explicit Reporter(TraceCallback callback) : callback_(std::move(callback)) {}
    bool enabled() const noexcept { return static_cast<bool>(callback_); }

    void emit(Operation operation, const Bytes& state,
              const char* description, std::size_t block = 0,
              const std::string& branch = {},
              std::optional<int> round = std::nullopt,
              const Bytes& before = {}, const Bytes& data = {}) {
        if (!callback_) return;
        Step step;
        step.sequence = ++sequence_;
        step.block_index = block;
        step.branch = branch;
        step.round = round;
        step.operation = operation;
        step.name = operation_name(operation);
        step.description = description;
        step.state = state;
        step.before = before;
        step.data = data;
        callback_(step);
    }

private:
    TraceCallback callback_;
    std::size_t sequence_ = 0;
};

// В GF(2^8) сложение — XOR. Умножение выполняется по модулю
// x^8 + x^4 + x^3 + x^2 + 1 = 0x11D (после сдвига остаётся 0x1D).
std::uint8_t multiply_gf(std::uint8_t a, std::uint8_t b) noexcept {
    std::uint8_t result = 0;
    for (int bit = 0; bit < 8; ++bit) {
        if (b & 1U) result ^= a;
        const bool highest_bit = (a & 0x80U) != 0;
        a = static_cast<std::uint8_t>(a << 1U);
        if (highest_bit) a ^= 0x1DU;
        b = static_cast<std::uint8_t>(b >> 1U);
    }
    return result;
}

std::uint64_t read_le64(const std::uint8_t* bytes) noexcept {
    std::uint64_t word = 0;
    for (std::size_t row = 0; row < ROWS; ++row)
        word |= static_cast<std::uint64_t>(bytes[row]) << (row * 8U);
    return word;
}

void write_le64(std::uint64_t word, std::uint8_t* bytes) noexcept {
    for (std::size_t row = 0; row < ROWS; ++row) {
        bytes[row] = static_cast<std::uint8_t>(word & 0xFFU);
        word >>= 8U;
    }
}

void add_constant(Bytes& state, const Params& params, int round, bool is_q,
                  Bytes* constants) {
    if (constants) constants->assign(params.block_bytes, 0);
    for (int column = 0; column < params.columns; ++column) {
        const std::size_t offset = static_cast<std::size_t>(column) * ROWS;
        if (!is_q) {
            // P: XOR константы только с байтом нулевой строки.
            const auto constant = static_cast<std::uint8_t>((column * 0x10) ^ round);
            state[offset] ^= constant;
            if (constants) (*constants)[offset] = constant;
        } else {
            // Q: каждую колонку читаем как LE-слово и прибавляем по модулю 2^64.
            // Это именно сложение с переносами, а не побайтовый XOR.
            const std::uint64_t high =
                (static_cast<std::uint64_t>(params.columns - column - 1) * 0x10U)
                ^ static_cast<std::uint64_t>(round);
            const std::uint64_t constant = 0x00F0F0F0F0F0F0F3ULL ^ (high << 56U);
            write_le64(read_le64(state.data() + offset) + constant,
                       state.data() + offset);
            if (constants) write_le64(constant, constants->data() + offset);
        }
    }
}

void sub_bytes(Bytes& state, const Params& params) {
    for (int column = 0; column < params.columns; ++column)
        for (std::size_t row = 0; row < ROWS; ++row) {
            const std::size_t offset = static_cast<std::size_t>(column) * ROWS + row;
            state[offset] = detail::SBOX[row % 4][state[offset]];
        }
}

void shift_rows(Bytes& state, const Params& params) {
    std::array<std::uint8_t, 16> shifted{};
    for (std::size_t row = 0; row < ROWS; ++row) {
        // Циклический сдвиг ВПРАВО: 0,1,...,7 для 8 колонок;
        // 0,1,2,3,4,5,6,11 для 16 колонок.
        const std::size_t shift = (params.columns == 16 && row == 7) ? 11 : row;
        const auto columns = static_cast<std::size_t>(params.columns);
        for (std::size_t column = 0; column < columns; ++column)
            shifted[(column + shift) % columns] = state[column * ROWS + row];
        for (std::size_t column = 0; column < columns; ++column)
            state[column * ROWS + row] = shifted[column];
    }
}

void mix_columns(Bytes& state, const Params& params) {
    for (int column = 0; column < params.columns; ++column) {
        std::array<std::uint8_t, ROWS> mixed{};
        const auto offset = static_cast<std::size_t>(column) * ROWS;
        for (std::size_t row = 0; row < ROWS; ++row)
            for (std::size_t item = 0; item < ROWS; ++item)
                mixed[row] ^= multiply_gf(detail::MDS[row][item], state[offset + item]);
        std::copy(mixed.begin(), mixed.end(), state.begin() + offset);
    }
}

void permute(Bytes& state, const Params& params, bool is_q,
             Reporter& report, std::size_t block, const std::string& branch) {
    for (int round = 0; round < params.rounds; ++round) {
        Bytes before;
        Bytes constants;
        if (report.enabled()) before = state;
        add_constant(state, params, round, is_q, report.enabled() ? &constants : nullptr);
        report.emit(Operation::AddRoundConstant, state,
                    is_q ? "Q: додаємо константи до LE-стовпців за модулем 2^64."
                         : "P: виконуємо XOR байтів рядка 0 з (16 * column) XOR round.",
                    block, branch, round, before, constants);

        if (report.enabled()) before = state;
        sub_bytes(state, params);
        report.emit(Operation::SubBytes, state,
                    "Замінюємо кожен байт за S-box з індексом row mod 4.",
                    block, branch, round, before);

        if (report.enabled()) before = state;
        shift_rows(state, params);
        report.emit(Operation::ShiftRows, state,
                    params.columns == 16
                        ? "Зсуваємо рядки вправо на 0,1,2,3,4,5,6,11 стовпців."
                        : "Зсуваємо рядки вправо на 0,1,2,3,4,5,6,7 стовпців.",
                    block, branch, round, before);

        if (report.enabled()) before = state;
        mix_columns(state, params);
        report.emit(Operation::MixColumns, state,
                    "Множимо кожен стовпець на MDS-матрицю в GF(2^8), поліном 0x11D.",
                    block, branch, round, before);
    }
}

Bytes encode_length(std::size_t bits) {
    Bytes length(LENGTH_BYTES, 0);
    // Последовательные сдвиги исключают сдвиг на ширину size_t и больше.
    for (std::size_t byte = 0; byte < LENGTH_BYTES && bits != 0; ++byte) {
        length[byte] = static_cast<std::uint8_t>(bits & 0xFFU);
        bits >>= 8U;
    }
    return length;
}

Bytes padding_tail(const Bytes& message, std::size_t bits, const Params& params) {
    const std::size_t full_bytes = bits / 8;
    const auto extra_bits = static_cast<unsigned>(bits % 8);
    const std::size_t first_tail_byte = (full_bytes / params.block_bytes) * params.block_bytes;
    Bytes tail(message.begin() + first_tail_byte, message.begin() + full_bytes);
    if (extra_bits != 0) {
        // В неполном байте оставляем только старшие биты сообщения и ставим 1.
        const auto mask = static_cast<std::uint8_t>(0xFFU << (8U - extra_bits));
        const auto marker = static_cast<std::uint8_t>(0x80U >> extra_bits);
        tail.push_back(static_cast<std::uint8_t>((message[full_bytes] & mask) | marker));
    } else {
        tail.push_back(0x80U);
    }
    const std::size_t padded_bytes =
        ((tail.size() + LENGTH_BYTES + params.block_bytes - 1) / params.block_bytes)
        * params.block_bytes;
    tail.resize(padded_bytes, 0);
    const Bytes length = encode_length(bits);
    std::copy(length.begin(), length.end(), tail.end() - LENGTH_BYTES);
    return tail;
}

void compress(Bytes& h, const Bytes& block, const Params& params,
              Reporter& report, std::size_t block_index) {
    report.emit(Operation::Block, block,
                "Черговий блок m_i; байти заповнюють стовпці згори вниз.", block_index);
    Bytes p = h;
    for (std::size_t byte = 0; byte < params.block_bytes; ++byte) p[byte] ^= block[byte];
    report.emit(Operation::XorInput, p,
                "Вхід P: h_(i-1) XOR m_i.", block_index, "P", std::nullopt, h, block);
    permute(p, params, false, report, block_index, "P");

    Bytes q = block;
    report.emit(Operation::PermutationInput, q,
                "Вхід Q: сам блок m_i.", block_index, "Q");
    permute(q, params, true, report, block_index, "Q");

    Bytes previous;
    Bytes operands;
    if (report.enabled()) {
        previous = h;
        operands = p;
        operands.insert(operands.end(), q.begin(), q.end());
    }
    for (std::size_t byte = 0; byte < params.block_bytes; ++byte)
        h[byte] ^= p[byte] ^ q[byte];
    report.emit(Operation::Compression, h,
                "h_i = h_(i-1) XOR P(h_(i-1) XOR m_i) XOR Q(m_i). Дані: P || Q.",
                block_index, {}, std::nullopt, previous, operands);
}

} // namespace

Params get_params(int bits) {
    if (bits == 256) return {256, 512, 8, 10, 64, 32};
    if (bits == 512) return {512, 1024, 16, 14, 128, 64};
    throw std::invalid_argument("У цій програмі Купина підтримує геші довжиною 256 або 512 біт.");
}

const char* operation_name(Operation operation) noexcept {
    switch (operation) {
        case Operation::Input: return "Input";
        case Operation::Padding: return "Padding";
        case Operation::Initialize: return "Initialize";
        case Operation::Block: return "Block";
        case Operation::XorInput: return "XorInput";
        case Operation::PermutationInput: return "PermutationInput";
        case Operation::AddRoundConstant: return "AddRoundConstant";
        case Operation::SubBytes: return "SubBytes";
        case Operation::ShiftRows: return "ShiftRows";
        case Operation::MixColumns: return "MixColumns";
        case Operation::Compression: return "Compression";
        case Operation::OutputXor: return "OutputXor";
        case Operation::Truncate: return "Truncate";
    }
    return "Unknown";
}

Bytes hash(const Bytes& message, int bits, std::optional<std::size_t> message_bits,
           TraceCallback trace) {
    const Params params = get_params(bits);
    std::size_t input_bits;
    if (message_bits) {
        input_bits = *message_bits;
        // Проверка без потенциально переполняющегося message.size() * 8.
        if (input_bits / 8 > message.size()
            || (input_bits / 8 == message.size() && input_bits % 8 != 0))
            throw std::invalid_argument("Довжина message_bits перевищує кількість наданих вхідних бітів.");
    } else {
        if (message.size() > std::numeric_limits<std::size_t>::max() / 8)
            throw std::length_error("Довжина вхідних даних у бітах не вміщується в size_t.");
        input_bits = message.size() * 8;
    }
    Reporter report(std::move(trace));

    if (report.enabled()) {
        const std::size_t used_bytes = input_bits / 8 + (input_bits % 8 != 0);
        const Bytes preview(message.begin(),
                            message.begin() + std::min(used_bytes, params.block_bytes));
        report.emit(Operation::Input, preview,
                    "Вхід: перші байти повідомлення (не більше одного блока). Дані: довжина у бітах, 96-bit LE.",
                    0, {}, std::nullopt, {}, encode_length(input_bits));
    }

    const Bytes tail = padding_tail(message, input_bits, params);
    report.emit(Operation::Padding, tail,
                "Останні 1–2 блоки: залишок повідомлення || 1 || нулі || довжина в 96-bit LE.",
                0, {}, std::nullopt, {}, report.enabled() ? encode_length(input_bits) : Bytes{});

    Bytes h(params.block_bytes, 0);
    h[0] = static_cast<std::uint8_t>(params.block_bytes);
    report.emit(Operation::Initialize, h,
                "h_0: перший байт дорівнює розміру стану в байтах (0x40 або 0x80), решта — нульові.");

    const std::size_t complete_blocks = (input_bits / 8) / params.block_bytes;
    std::size_t block_index = 0;
    for (std::size_t index = 0; index < complete_blocks; ++index) {
        const std::size_t offset = index * params.block_bytes;
        const Bytes block(message.begin() + offset, message.begin() + offset + params.block_bytes);
        compress(h, block, params, report, ++block_index);
    }
    for (std::size_t offset = 0; offset < tail.size(); offset += params.block_bytes) {
        const Bytes block(tail.begin() + offset, tail.begin() + offset + params.block_bytes);
        compress(h, block, params, report, ++block_index);
    }

    Bytes p = h;
    report.emit(Operation::PermutationInput, p,
                "Вихідне перетворення: виконуємо P(h_N).", 0, "output");
    permute(p, params, false, report, 0, "output");
    Bytes previous;
    if (report.enabled()) previous = h;
    for (std::size_t byte = 0; byte < params.block_bytes; ++byte) h[byte] ^= p[byte];
    report.emit(Operation::OutputXor, h,
                "Фінальний стан: h_N XOR P(h_N).", 0, "output", std::nullopt, previous, p);

    // Стандарт требует последние n/8 байтов, а не первые.
    const Bytes digest(h.end() - params.output_bytes, h.end());
    report.emit(Operation::Truncate, digest,
                "Беремо останні 32 або 64 байти фінального стану — геш повідомлення.",
                0, "output", std::nullopt, h, digest);
    return digest;
}

} // namespace kupyna
