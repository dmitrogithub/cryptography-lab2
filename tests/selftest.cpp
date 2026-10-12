#include "selftest.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace kupyna::tests {
namespace {

// Источник опубликованных векторов: Kupyna-reference/main.c из задания.
// Авторы эталонной реализации: Ruslan Kiianchuk, Ruslan Mordvinov,
// Roman Oliynykov. Хеши границ padding отдельно получены её kupyna.c/tables.c.
Bytes sequence(std::size_t bytes) {
    Bytes result(bytes);
    for (std::size_t i = 0; i < bytes; ++i)
        result[i] = static_cast<std::uint8_t>(i & 0xFFU);
    return result;
}

Bytes sequence_bits(std::size_t bits) {
    Bytes result = sequence(bits / 8 + (bits % 8 != 0));
    if (bits % 8 != 0)
        result.back() &= static_cast<std::uint8_t>(0xFFU << (8U - bits % 8));
    return result;
}

std::string hex(const Bytes& bytes) {
    std::ostringstream result;
    result << std::uppercase << std::hex << std::setfill('0');
    for (auto byte : bytes) result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_digest(const Bytes& digest, const std::string& expected) {
    require(hex(digest) == expected, "expected " + expected + ", got " + hex(digest));
}

class Suite {
public:
    explicit Suite(std::ostream& output) : output_(output) {}

    template<class Function>
    void run(const std::string& name, Function&& function) {
        ++total_;
        try {
            function();
            output_ << "[OK] " << name << '\n';
        } catch (const std::exception& error) {
            ++failed_;
            output_ << "[FAIL] " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed_;
            output_ << "[FAIL] " << name << ": unknown exception\n";
        }
    }

    bool result() {
        output_ << "Self-test: " << (total_ - failed_) << '/' << total_
                << " passed; " << failed_ << " failed.\n";
        return failed_ == 0;
    }

private:
    std::ostream& output_;
    std::size_t total_ = 0;
    std::size_t failed_ = 0;
};

Bytes length_field(std::size_t bits) {
    Bytes bytes(12, 0);
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(bits & 0xFFU);
        bits >>= 8U;
    }
    return bytes;
}

// Ожидаемое дополнение конструируется из определения M || 1 || 0^k || len96,
// чтобы тест обнаруживал ошибку именно на границе 1/2 дополнительных блоков.
Bytes expected_tail(const Bytes& message, std::size_t bits, std::size_t block_bytes) {
    const std::size_t full_blocks = bits / (8 * block_bytes);
    const std::size_t tail_bits = bits - full_blocks * 8 * block_bytes;
    const std::size_t padded_blocks = (tail_bits + 97 <= block_bytes * 8) ? 1 : 2;
    Bytes tail(padded_blocks * block_bytes, 0);
    const std::size_t whole_bytes = tail_bits / 8;
    std::copy_n(message.begin() + full_blocks * block_bytes, whole_bytes, tail.begin());
    const auto partial_bits = static_cast<unsigned>(tail_bits % 8);
    if (partial_bits != 0) {
        const auto mask = static_cast<std::uint8_t>(0xFFU << (8U - partial_bits));
        tail[whole_bytes] = static_cast<std::uint8_t>(
            (message[full_blocks * block_bytes + whole_bytes] & mask)
            | (0x80U >> partial_bits));
    } else {
        tail[whole_bytes] = 0x80;
    }
    const auto length = length_field(bits);
    std::copy(length.begin(), length.end(), tail.end() - 12);
    return tail;
}

Bytes xor_bytes(const Bytes& a, const Bytes& b) {
    require(a.size() == b.size(), "XOR operands have different sizes");
    Bytes result(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) result[i] = a[i] ^ b[i];
    return result;
}

class TraceReader {
public:
    TraceReader(const std::vector<Step>& steps, const Params& params)
        : steps_(steps), params_(params) {}

    const Step& take(Operation operation, std::size_t block = 0,
                     const std::string& branch = {}, std::optional<int> round = std::nullopt) {
        require(position_ < steps_.size(), "trace ended before " + std::string(operation_name(operation)));
        const auto& step = steps_[position_++];
        require(step.sequence == position_, "trace sequence must start at 1 and stay consecutive");
        require(step.operation == operation, "wrong operation at step " + std::to_string(position_));
        require(step.block_index == block && step.branch == branch && step.round == round,
                "wrong block, branch or round metadata at step " + std::to_string(position_));
        require(!step.name.empty() && !step.description.empty(), "step lacks its teaching explanation");
        return step;
    }

    Bytes permutation(Bytes state, std::size_t block, const std::string& branch) {
        constexpr std::array<Operation, 4> operations = {
            Operation::AddRoundConstant, Operation::SubBytes,
            Operation::ShiftRows, Operation::MixColumns};
        for (int round = 0; round < params_.rounds; ++round) {
            for (const auto operation : operations) {
                const auto& step = take(operation, block, branch, round);
                require(step.before == state, "before snapshot breaks permutation continuity");
                require(step.state.size() == params_.block_bytes, "round matrix has incorrect size");
                if (operation == Operation::AddRoundConstant)
                    check_constant(step, branch == "Q", round);
                if (operation == Operation::ShiftRows)
                    check_shift(step);
                state = step.state;
            }
        }
        return state;
    }

    void done() const {
        require(position_ == steps_.size(), "unexpected extra trace events");
    }

private:
    void check_constant(const Step& step, bool q, int round) const {
        Bytes expected_constants(params_.block_bytes, 0);
        Bytes expected_state = step.before;
        for (int column = 0; column < params_.columns; ++column) {
            const auto offset = static_cast<std::size_t>(column) * 8;
            if (!q) {
                const auto constant = static_cast<std::uint8_t>((column * 16) ^ round);
                expected_constants[offset] = constant;
                expected_state[offset] ^= constant;
            } else {
                // Независимая побайтовая проверка сложения с переносами в LE-колонке.
                expected_constants[offset] = 0xF3;
                for (std::size_t row = 1; row < 7; ++row)
                    expected_constants[offset + row] = 0xF0;
                expected_constants[offset + 7] = static_cast<std::uint8_t>(
                    ((params_.columns - column - 1) * 16) ^ round);
                unsigned carry = 0;
                for (std::size_t row = 0; row < 8; ++row) {
                    const unsigned sum = expected_state[offset + row]
                                         + expected_constants[offset + row] + carry;
                    expected_state[offset + row] = static_cast<std::uint8_t>(sum & 0xFFU);
                    carry = sum >> 8U;
                }
            }
        }
        require(step.data == expected_constants, "round constants are not the standard constants");
        require(step.state == expected_state, "constant trace does not match its stated operation");
    }

    void check_shift(const Step& step) const {
        const auto columns = static_cast<std::size_t>(params_.columns);
        for (std::size_t row = 0; row < 8; ++row) {
            const std::size_t shift = (columns == 16 && row == 7) ? 11 : row;
            for (std::size_t column = 0; column < columns; ++column) {
                const auto source_column = (column + columns - shift) % columns;
                require(step.state[column * 8 + row] == step.before[source_column * 8 + row],
                        "ShiftRows snapshot does not show the required cyclic right shift");
            }
        }
    }

    const std::vector<Step>& steps_;
    const Params& params_;
    std::size_t position_ = 0;
};

void check_trace(const Bytes& message, int digest_bits, std::size_t bits,
                 const Bytes& digest, const std::vector<Step>& steps) {
    const Params params = get_params(digest_bits);
    TraceReader reader(steps, params);
    const auto& input = reader.take(Operation::Input);
    require(input.data == length_field(bits), "Input does not record the bit length");
    const auto used_bytes = bits / 8 + (bits % 8 != 0);
    const Bytes preview(message.begin(), message.begin() + std::min(used_bytes, params.block_bytes));
    require(input.state == preview, "Input preview does not correspond to the message");
    const auto& padding = reader.take(Operation::Padding);
    const Bytes tail = expected_tail(message, bits, params.block_bytes);
    require(padding.state == tail, "padding marker, zero fill or 96-bit LE length is wrong");
    require(padding.data == length_field(bits), "Padding length field is wrong");
    const auto& initial = reader.take(Operation::Initialize);
    Bytes h(params.block_bytes, 0);
    h[0] = static_cast<std::uint8_t>(params.block_bytes);
    require(initial.state == h, "incorrect h_0 matrix");

    const auto complete_blocks = bits / (8 * params.block_bytes);
    const auto total_blocks = complete_blocks + tail.size() / params.block_bytes;
    for (std::size_t block_index = 1; block_index <= total_blocks; ++block_index) {
        const Bytes& source = block_index <= complete_blocks ? message : tail;
        const auto offset = (block_index <= complete_blocks ? block_index - 1
                                                            : block_index - complete_blocks - 1)
                            * params.block_bytes;
        const Bytes block(source.begin() + offset, source.begin() + offset + params.block_bytes);
        require(reader.take(Operation::Block, block_index).state == block,
                "trace shows the wrong message block");
        const auto& xor_input = reader.take(Operation::XorInput, block_index, "P");
        require(xor_input.before == h && xor_input.data == block
                && xor_input.state == xor_bytes(h, block), "P input must equal h XOR m");
        const Bytes p = reader.permutation(xor_input.state, block_index, "P");
        const auto& q_input = reader.take(Operation::PermutationInput, block_index, "Q");
        require(q_input.state == block, "Q input must equal m");
        const Bytes q = reader.permutation(q_input.state, block_index, "Q");
        const auto& compression = reader.take(Operation::Compression, block_index);
        Bytes operands = p;
        operands.insert(operands.end(), q.begin(), q.end());
        require(compression.before == h && compression.data == operands,
                "compression does not show h and P || Q operands");
        h = xor_bytes(xor_bytes(h, p), q);
        require(compression.state == h, "compression trace must equal h XOR P XOR Q");
    }

    const auto& output_input = reader.take(Operation::PermutationInput, 0, "output");
    require(output_input.state == h, "output P must start from h_N");
    const Bytes p = reader.permutation(h, 0, "output");
    const auto& output_xor = reader.take(Operation::OutputXor, 0, "output");
    require(output_xor.before == h && output_xor.data == p
            && output_xor.state == xor_bytes(h, p), "output transformation must equal h_N XOR P(h_N)");
    const auto& truncate = reader.take(Operation::Truncate, 0, "output");
    const Bytes expected_digest(output_xor.state.end() - params.output_bytes, output_xor.state.end());
    require(truncate.before == output_xor.state && truncate.state == expected_digest
            && digest == expected_digest, "Truncate must select the last digest bytes");
    reader.done();
}

void traced_case(const Bytes& message, int digest_bits, std::size_t bits,
                 const std::string& expected = {}) {
    std::vector<Step> steps;
    const auto digest = hash(message, digest_bits, bits, [&](const Step& step) { steps.push_back(step); });
    require(digest == hash(message, digest_bits, bits), "tracing changes the hash");
    if (!expected.empty()) require_digest(digest, expected);
    check_trace(message, digest_bits, bits, digest, steps);
}

template<class Function>
void requires_invalid_argument(Function&& function) {
    bool rejected = false;
    try { function(); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid input must throw std::invalid_argument");
}

struct BoundaryVector {
    int digest_bits;
    std::size_t message_bytes;
    const char* expected;
};

constexpr std::array<BoundaryVector, 8> boundaries = {{
    {256, 51, "6F8F0A3F8261AF77581AB01CB89D4CB5ED87CA1D9954F11D5586E94B45C82FB8"},
    {256, 52, "8B6FE2BA77E684B2A1AC82232F4EFC49F681CD18C82A0CFFF530186A2FC642D2"},
    {256, 63, "03A44A02C9FFAFB43ADDB290BBCF3B8168F624E8CBD332DC6A9DC7DF9D39CBC2"},
    {256, 64, "08F4EE6F1BE6903B324C4E27990CB24EF69DD58DBE84813EE0A52F6631239875"},
    {512, 115, "0CE7A17A34C4785DA207DA4AC39A54F2819F24AD56CE42C76A59ADC36672E71B15FBD5D3E1DDFAE96FD88BD860FE4A1CDAE8D2465B5E9238FD3EB695A69B4B2F"},
    {512, 116, "306BB714AED5988733A6011062004452C55B19C315797F822744025D4DE8BAB88D3CDB9DB9C965D5DB4866DCEDD485B407A95F2C2E6340774D8993B9D9FE505E"},
    {512, 127, "45D00504BC31F2545B2A5C9742CE26118CF0DE519DF78ECE9476193347AB5ACD3FF0C3633AB78D9CD8DC6EF02D2343F128612A00952C9B17AD719A382ECC19FF"},
    {512, 128, "76ED1AC28B1D0143013FFA87213B4090B356441263C13E03FA060A8CADA32B979635657F256B15D5FCA4A174DE029F0B1B4387C878FCC1C00E8705D783FD7FFE"}
}};

} // namespace

const std::vector<TestVector>& reference_vectors() {
    static const std::vector<TestVector> vectors = {
        {"Kupyna-256 / N=512", 256, 512, sequence_bits(512), "08F4EE6F1BE6903B324C4E27990CB24EF69DD58DBE84813EE0A52F6631239875"},
        {"Kupyna-256 / N=1024", 256, 1024, sequence_bits(1024), "0A9474E645A7D25E255E9E89FFF42EC7EB31349007059284F0B182E452BDA882"},
        {"Kupyna-256 / N=2048", 256, 2048, sequence_bits(2048), "D305A32B963D149DC765F68594505D4077024F836C1BF03806E1624CE176C08F"},
        {"Kupyna-256 / FF (8 bits)", 256, 8, {0xFF}, "EA7677CA4526555680441C117982EA14059EA6D0D7124D6ECDB3DEEC49E890F4"},
        {"Kupyna-256 / N=760", 256, 760, sequence_bits(760), "1075C8B0CB910F116BDA5FA1F19C29CF8ECC75CAFF7208BA2994B68FC56E8D16"},
        {"Kupyna-256 / empty", 256, 0, {}, "CD5101D1CCDF0D1D1F4ADA56E888CD724CA1A0838A3521E7131D4FB78D0F5EB6"},
        {"Kupyna-256 / N=510", 256, 510, sequence_bits(510), "875C0023DAA0C077809FDD6A9672B49E03903BFF98EBE48740AE998C7BE3851E"},
        {"Kupyna-256 / N=655", 256, 655, sequence_bits(655), "4237D7DE1A00C4CC8037EDE9C54BA60D1C705CD1495DE19E5245BF3509DB59CE"},
        {"Kupyna-512 / N=512", 512, 512, sequence_bits(512), "3813E2109118CDFB5A6D5E72F7208DCCC80A2DFB3AFDFB02F46992B5EDBE536B3560DD1D7E29C6F53978AF58B444E37BA685C0DD910533BA5D78EFFFC13DE62A"},
        {"Kupyna-512 / N=1024", 512, 1024, sequence_bits(1024), "76ED1AC28B1D0143013FFA87213B4090B356441263C13E03FA060A8CADA32B979635657F256B15D5FCA4A174DE029F0B1B4387C878FCC1C00E8705D783FD7FFE"},
        {"Kupyna-512 / N=2048", 512, 2048, sequence_bits(2048), "0DD03D7350C409CB3C29C25893A0724F6B133FA8B9EB90A64D1A8FA93B56556611EB187D715A956B107E3BFC76482298133A9CE8CBC0BD5E1436A5B197284F7E"},
        {"Kupyna-512 / FF (8 bits)", 512, 8, {0xFF}, "871B18CF754B72740307A97B449ABEB32B64444CC0D5A4D65830AE5456837A72D8458F12C8F06C98C616ABE11897F86263B5CB77C420FB375374BEC52B6D0292"},
        {"Kupyna-512 / N=1536", 512, 1536, sequence_bits(1536), "B189BFE987F682F5F167F0D7FA565330E126B6E592B1C55D44299064EF95B1A57F3C2D0ECF17869D1D199EBBD02E8857FB8ADD67A8C31F56CD82C016CF743121"},
        {"Kupyna-512 / empty", 512, 0, {}, "656B2F4CD71462388B64A37043EA55DBE445D452AECD46C3298343314EF04019BCFA3F04265A9857F91BE91FCE197096187CEDA78C9C1C021C294A0689198538"},
        {"Kupyna-512 / N=510", 512, 510, sequence_bits(510), "2F3BBAC98E8771D6E3B8AA30153ABC4D0C2985E91DA1B5568FD1BDD705CCAB7EE8D95D2FC98BFA5322A241E09C896B58284C83F2488CF943E4B3DE43E05F0DEA"},
        {"Kupyna-512 / N=655", 512, 655, sequence_bits(655), "01B7BDA1DBA77D7379F53C2A498A390DE5E688A12BC75FEE9E010CB6FEBED3B9C7023931C74A7B55168A15047D5E2CB78A8B5CA2F75E05E80CA398030E02C7AA"}
    };
    return vectors;
}

bool run_self_tests(std::ostream& output) {
    Suite suite(output);
    for (const auto& vector : reference_vectors()) {
        suite.run(vector.name, [&] {
            require(vector.message.size() == vector.message_bits / 8 + (vector.message_bits % 8 != 0),
                    "fixture byte size must be ceil(message_bits / 8)");
            require_digest(hash(vector.message, vector.digest_bits, vector.message_bits), vector.expected_hex);
        });
    }

    for (const auto& vector : boundaries) {
        suite.run("Padding boundary / Kupyna-" + std::to_string(vector.digest_bits)
                  + " / " + std::to_string(vector.message_bytes) + " bytes", [&] {
            traced_case(sequence(vector.message_bytes), vector.digest_bits, vector.message_bytes * 8, vector.expected);
        });
    }

    const Bytes utf8 = {0xD0, 0x9A, 0xD1, 0x83, 0xD0, 0xBF, 0xD0, 0xB8, 0xD0, 0xBD, 0xD0, 0xB0};
    suite.run("UTF-8 bytes / Kupyna-256", [&] {
        require_digest(hash(utf8, 256), "6B20BB881098EBEFB147ED37A56D2808B078D4782C354AD66E06EC2C65D9576D");
    });
    suite.run("UTF-8 bytes / Kupyna-512", [&] {
        require_digest(hash(utf8, 512), "C748BB37DB695CA3A509D7B89B4C1F253331580AFA8FB78C0408E090D1B189AC4580298C7B8EB7C1574626FD4BEF892F9993CEDB344BC021297F7729C0498B0D");
    });

    for (const int bits : {256, 512}) {
        suite.run("Full trace / Kupyna-" + std::to_string(bits) + " / multiblock", [&] {
            traced_case(sequence(256), bits, 2048);
        });
        suite.run("Partial-bit trace / Kupyna-" + std::to_string(bits), [&] {
            traced_case(sequence(64), bits, 510);
            traced_case(sequence(82), bits, 655);
            traced_case({0xFF}, bits, 1);
            traced_case({}, bits, 0);
        });
        suite.run("Unused bits and suffix ignored / Kupyna-" + std::to_string(bits), [&] {
            for (const std::size_t length : {std::size_t{1}, std::size_t{7}, std::size_t{510}, std::size_t{655}}) {
                Bytes original = sequence_bits(length);
                Bytes changed = original;
                changed.back() |= static_cast<std::uint8_t>((1U << (8U - length % 8)) - 1U);
                changed.insert(changed.end(), {0xDE, 0xAD, 0xBE, 0xEF});
                require(hash(original, bits, length) == hash(changed, bits, length),
                        "unused low bits or trailing bytes affect digest");
            }
            require(hash({0xFF, 0x00}, bits, 0) == hash({}, bits), "explicit zero bit length must hash the empty message");
        });
        suite.run("Default length and immutable input / Kupyna-" + std::to_string(bits), [&] {
            const Bytes message = sequence(128);
            const Bytes saved = message;
            require(hash(message, bits) == hash(message, bits, 1024), "default length does not use all bytes");
            require(message == saved, "hash modified caller's bytes");
        });
    }

    suite.run("Reject unsupported digest sizes", [&] {
        for (const int bits : {-1, 0, 1, 255, 384, 513})
            requires_invalid_argument([&] { hash({}, bits); });
    });
    suite.run("Reject bit length beyond available bytes", [&] {
        for (const int digest_bits : {256, 512}) {
            requires_invalid_argument([&] { hash({}, digest_bits, 1); });
            requires_invalid_argument([&] { hash({0xFF}, digest_bits, 9); });
            requires_invalid_argument([&] { hash({0xFF}, digest_bits, std::numeric_limits<std::size_t>::max()); });
        }
    });
    suite.run("Rejected input emits no trace", [&] {
        std::size_t callbacks = 0;
        requires_invalid_argument([&] {
            hash({0xFF}, 256, 9, [&](const Step&) { ++callbacks; });
        });
        require(callbacks == 0, "an invalid request emitted misleading trace steps");
    });
    suite.run("Callback exception propagates and later hashes still work", [&] {
        bool caught = false;
        try {
            hash({}, 256, std::nullopt, [](const Step&) { throw std::logic_error("stop trace"); });
        } catch (const std::logic_error&) { caught = true; }
        require(caught, "callback exception was silently swallowed");
        const auto& empty = reference_vectors()[5];
        require_digest(hash({}, 256), empty.expected_hex);
    });

    return suite.result();
}

} // namespace kupyna::tests

bool run_self_tests(std::ostream& output) {
    return kupyna::tests::run_self_tests(output);
}
