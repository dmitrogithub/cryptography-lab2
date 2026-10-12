#include "kupyna.hpp"
#include "selftest.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using kupyna::Bytes;
constexpr std::size_t TRACE_LIMIT = 1024;
constexpr std::size_t FILE_LIMIT = 16 * 1024 * 1024;

struct Cancelled : std::exception {};
struct EndOfInput : std::exception {};
struct Input {
    Bytes bytes;
    std::optional<std::size_t> bits;
    std::string label;
    std::string source_file;
};

std::string hex(const Bytes& bytes) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (auto byte : bytes) out << std::setw(2) << unsigned(byte);
    return out.str();
}

std::size_t number(const std::string& text, const char* name) {
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument(std::string(name) + ": очікується невід'ємне ціле число.");
    return value;
}

Bytes parse_hex(const std::string& text) {
    std::string digits;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            throw std::invalid_argument("HEX: дозволені 0–9, A–F та пробіли; префікс 0x не потрібний.");
        digits.push_back(static_cast<char>(c));
    }
    if (digits.size() % 2) throw std::invalid_argument("HEX: кожен байт повинен мати дві цифри.");
    auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    Bytes bytes;
    bytes.reserve(digits.size() / 2);
    for (std::size_t i = 0; i < digits.size(); i += 2)
        bytes.push_back(static_cast<std::uint8_t>((nibble(digits[i]) << 4) | nibble(digits[i + 1])));
    return bytes;
}

Bytes read_file(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Не вдалося відкрити файл: " + path);
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > FILE_LIMIT)
        throw std::runtime_error("Для навчальної програми розмір файлу обмежений 16 MiB.");
    Bytes bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Не вдалося прочитати файл повністю: " + path);
    return bytes;
}

std::string ask(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string value;
    if (!std::getline(std::cin, value)) throw EndOfInput{};
    if (!value.empty() && value.back() == '\r') value.pop_back();
    return value;
}

std::size_t input_bits(const Input& input) { return input.bits.value_or(input.bytes.size() * 8); }

void validate_trace_destination(const Input& input, int bits, const std::string& path) {
    kupyna::get_params(bits);
    const auto n = input_bits(input);
    if (n / 8 > input.bytes.size() || (n / 8 == input.bytes.size() && n % 8))
        throw std::invalid_argument("Довжина у бітах перевищує введені дані.");
    if (n / 8 + (n % 8 != 0) > TRACE_LIMIT)
        throw std::invalid_argument("Покрокова траса обмежена 1024 байтами.");
    if (!input.source_file.empty() && std::filesystem::exists(std::filesystem::u8path(path))
        && std::filesystem::equivalent(std::filesystem::u8path(input.source_file), std::filesystem::u8path(path)))
        throw std::invalid_argument("Файл траси повинен відрізнятися від вхідного файла.");
}

void print_bytes(std::ostream& out, const Bytes& bytes) {
    if (bytes.empty()) { out << "  (порожня послідовність)\n"; return; }
    const auto flags = out.flags();
    const auto fill = out.fill();
    out << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); i += 16) {
        out << "  " << std::setw(4) << i << ": ";
        for (std::size_t j = i; j < std::min(i + 16, bytes.size()); ++j)
            out << std::setw(2) << unsigned(bytes[j]) << ' ';
        out << '\n';
    }
    out.flags(flags); out.fill(fill);
}

void print_matrix(std::ostream& out, const Bytes& bytes, const Bytes& before, int columns) {
    if (bytes.size() % (8 * columns)) return;
    const auto flags = out.flags();
    const auto fill = out.fill();
    out << "  Матриця: s[row, col] = bytes[col * 8 + row]. * — байт змінився.\n";
    for (std::size_t offset = 0; offset < bytes.size(); offset += 8 * columns) {
        if (bytes.size() > static_cast<std::size_t>(8 * columns))
            out << "  Частина " << offset / (8 * columns) + 1 << ":\n";
        out << "       ";
        for (int col = 0; col < columns; ++col) out << std::dec << std::setfill(' ') << std::setw(4) << col;
        out << '\n';
        for (int row = 0; row < 8; ++row) {
            out << "  r" << std::dec << row << "   ";
            for (int col = 0; col < columns; ++col) {
                const auto i = offset + col * 8 + row;
                const bool changed = before.size() == bytes.size() && before[i] != bytes[i];
                out << (changed ? '*' : ' ') << std::hex << std::uppercase << std::setfill('0')
                    << std::setw(2) << unsigned(bytes[i]) << ' ';
            }
            out << '\n';
        }
    }
    out.flags(flags); out.fill(fill);
}

class TracePrinter {
    std::ostream& out_;
    kupyna::Params params_;
    bool pause_, matrix_;
public:
    TracePrinter(std::ostream& out, int bits, bool pause, bool matrix)
        : out_(out), params_(kupyna::get_params(bits)), pause_(pause), matrix_(matrix) {}
    void operator()(const kupyna::Step& step) {
        out_ << "\n[Крок " << step.sequence << "] " << step.name;
        if (step.block_index) out_ << " | блок " << step.block_index;
        if (!step.branch.empty()) out_ << " | " << (step.branch == "output" ? "фінальне P" : step.branch);
        if (step.round) out_ << " | раунд " << *step.round + 1 << '/' << params_.rounds
                             << " (round=" << *step.round << ')';
        out_ << '\n' << step.description << '\n';
        if (!step.before.empty()) {
            out_ << "До операції:\n"; print_bytes(out_, step.before);
        }
        if (!step.data.empty() && step.operation != kupyna::Operation::Truncate) {
            out_ << (step.operation == kupyna::Operation::AddRoundConstant ? "Раундова константа:\n"
                     : step.operation == kupyna::Operation::Compression ? "Операнди P || Q:\n"
                     : step.operation == kupyna::Operation::Padding || step.operation == kupyna::Operation::Input
                         ? "Довжина повідомлення у 96-бітному форматі little-endian:\n" : "Операнд:\n");
            print_bytes(out_, step.data);
        }
        out_ << (step.operation == kupyna::Operation::Truncate ? "Геш:\n" : "Після операції:\n");
        print_bytes(out_, step.state);
        if (matrix_ && step.operation != kupyna::Operation::Input && step.operation != kupyna::Operation::Truncate)
            print_matrix(out_, step.state, step.before, params_.columns);
        if (pause_) {
            const auto command = ask("Enter — наступний крок; a — решта без пауз; q — повернутися: ");
            if (command == "q" || command == "Q") throw Cancelled{};
            if (command == "a" || command == "A") pause_ = false;
        }
    }
};

void explain(std::ostream& out) {
    out << R"(Купина за ДСТУ 7564:2014 — криптографічна геш-функція.
Вона перетворює повідомлення довільної довжини на фіксований геш.
Для гешування ключ не потрібний; оберненого «розшифрування» немає.

                 Купина-256      Купина-512
Геш              32 байти        64 байти
Блок і стан      64 байти        128 байтів
Матриця          8 x 8           8 x 16
Раундів P/Q      10              14
IV               40 00...00      80 00...00
Зсуви вправо     0,1,2,3,4,5,6,7 0,1,2,3,4,5,6,11

1. Кодуємо текст у UTF-8 або вводимо конкретні байти через HEX.
2. Доповнюємо: M || 1 || 0^k || LE96(|M|).
   (|M| + 1 + k + 96) має ділитися на 512 або 1024.
   Для повних байтів маркер — 80. 12 останніх байтів містять довжину
   початкового повідомлення у бітах у little-endian.
3. h_0 = IV. Обробляємо доповнене повідомлення блок за блоком.
4. Для кожного m_i обчислюємо:
       h_i = P(h_(i-1) XOR m_i) XOR Q(m_i) XOR h_(i-1).
5. У кожному раунді P і Q послідовно виконуємо:
   - AddRoundConstant: P — XOR у рядку 0;
     Q — додавання до 64-бітових LE-стовпців за модулем 2^64.
   - SubBytes: заміна байтів чотирма S-box, номер = row mod 4.
   - ShiftRows: циклічний зсув рядків вправо.
   - MixColumns: множення стовпців на MDS-матрицю в GF(2^8),
     поліном x^8+x^4+x^3+x^2+1 (0x11D).
6. Обчислюємо P(h_N) XOR h_N. Беремо ОСТАННІ 32/64 байти.

Приклад abc: UTF-8 = 61 62 63, довжина = 24 біти.
Купина-256: 61 62 63 80, 48 нульових байтів, LE96(24) = 18 00...00.
Отримуємо один блок 64 байти; потім P, Q, стиснення і фінальне P.

У трасі байти йдуть стовпцями: byte[col * 8 + row].
Показаний номер раунду 1..10/14; round у формулі — 0..9/13.
Усі показані значення обчислюються під час виконання алгоритму.
Докладні формули, матриця MDS і джерела — у THEORY.md.
)";
}

void describe_input(std::ostream& out, const Input& input, int bits) {
    const auto p = kupyna::get_params(bits);
    const auto n = input_bits(input);
    if (n / 8 > input.bytes.size() || (n / 8 == input.bytes.size() && n % 8))
        throw std::invalid_argument("Довжина у бітах перевищує введені дані.");
    // Ліміт застосовується до використаного префікса, не до зайвих байтів.
    const auto blocks = n / p.state_bits + ((n % p.state_bits + 97 + p.state_bits - 1) / p.state_bits);
    out << "\nКупина-" << bits << " | " << input.label << " | " << n << " біт\n"
        << "Стан " << p.state_bits << " біт; " << p.columns << " стовпців; " << p.rounds
        << " раундів; блоків з доповненням: " << blocks << '\n';
}

Bytes calculate(std::ostream& out, const Input& input, int bits, bool trace, bool pause, bool matrix) {
    describe_input(out, input, bits);
    if (trace && (input_bits(input) / 8 + (input_bits(input) % 8 != 0)) > TRACE_LIMIT)
        throw std::invalid_argument("Покрокова траса обмежена 1024 байтами. Для більших даних використайте hash.");
    TracePrinter printer(out, bits, pause, matrix);
    const auto digest = kupyna::hash(input.bytes, bits, input.bits,
        trace ? kupyna::TraceCallback([&](const kupyna::Step& step) { printer(step); }) : kupyna::TraceCallback{});
    out << "\nРезультат Купина-" << bits << ":\n" << hex(digest) << "\n";
    return digest;
}

Input interactive_input() {
    const auto mode = ask("Вхід: 1 — текст UTF-8; 2 — HEX; 3 — порожній; 4 — 00..3F; 5 — файл [1]: ");
    Input input;
    if (mode.empty() || mode == "1") {
        auto text = ask("Текст [abc]: ");
        if (text.empty()) text = "abc";
        input = {Bytes(text.begin(), text.end()), {}, "текст UTF-8"};
    } else if (mode == "2") {
        input = {parse_hex(ask("HEX (наприклад 00 01 FF; Enter — порожній): ")), {}, "HEX"};
        const auto bits = ask("Кількість значущих бітів [усі]: ");
        if (!bits.empty()) input.bits = number(bits, "Кількість бітів");
    } else if (mode == "3") input = {{}, {}, "порожнє повідомлення"};
    else if (mode == "4") {
        input.label = "еталонні байти 00..3F";
        for (int i = 0; i < 64; ++i) input.bytes.push_back(static_cast<std::uint8_t>(i));
    } else if (mode == "5") {
        input.source_file = ask("Шлях до файлу: ");
        input.bytes = read_file(input.source_file);
        input.label = "файл";
    }
    else throw std::invalid_argument("Оберіть джерело від 1 до 5.");
    return input;
}

int menu() {
    for (;;) {
        try {
            std::cout << "\n=== ЛР2: Купина | ДСТУ 7564:2014 ===\n"
                         "1 — Купина-256 покроково\n2 — Купина-512 покроково\n"
                         "3 — Геш обох режимів без траси\n4 — Як працює алгоритм\n"
                         "5 — Перевірка реалізації\n0 — Вихід\n";
            const auto choice = ask("Ваш вибір: ");
            if (choice == "0") return 0;
            if (choice == "4") { explain(std::cout); continue; }
            if (choice == "5") { run_self_tests(std::cout); continue; }
            if (choice != "1" && choice != "2" && choice != "3") {
                std::cout << "Оберіть пункт 0..5.\n"; continue;
            }
            const auto input = interactive_input();
            if (choice == "3") {
                calculate(std::cout, input, 256, false, false, false);
                calculate(std::cout, input, 512, false, false, false);
                continue;
            }
            const int bits = choice == "1" ? 256 : 512;
            const bool pause = ask("Enter — пауза після кожного кроку; 0 — усі одразу: ") != "0";
            const bool matrix = ask("Матриця стану? Enter — так; 0 — лише HEX: ") != "0";
            const auto path = ask("Зберегти трасу? Шлях до .txt або Enter — лише екран: ");
            if (path.empty()) calculate(std::cout, input, bits, true, pause, matrix);
            else {
                validate_trace_destination(input, bits, path);
                std::ofstream file(std::filesystem::u8path(path), std::ios::binary);
                if (!file) throw std::runtime_error("Не вдалося створити файл траси.");
                // Один запуск алгоритму. Пауза управляє реальним наступним кроком.
                describe_input(std::cout, input, bits);
                describe_input(file, input, bits);
                TracePrinter screen(std::cout, bits, pause, matrix), disk(file, bits, false, matrix);
                auto digest = kupyna::hash(input.bytes, bits, input.bits, [&](const kupyna::Step& step) {
                    disk(step); screen(step);
                });
                file << "\nРезультат Купина-" << bits << ":\n" << hex(digest) << '\n';
                file.flush();
                if (!file) throw std::runtime_error("Помилка запису траси.");
                std::cout << "\nРезультат: " << hex(digest) << "\nТрасу збережено: " << path << '\n';
            }
        } catch (const EndOfInput&) { return 0; }
        catch (const Cancelled&) { std::cout << "\nПокроковий перегляд завершено користувачем.\n"; }
        catch (const std::exception& error) { std::cerr << "Помилка: " << error.what() << '\n'; }
    }
}

void help() {
    std::cout << R"(kupyna-lab — ЛР2, ДСТУ 7564:2014
Без аргументів: інтерактивне меню.
  explain
  selftest
  hash  --bits 256|512 (--text TEXT | --hex HEX | --file PATH) [--message-bits N]
  trace --bits 256|512 (--text TEXT | --hex HEX | --file PATH) [--message-bits N]
        [--matrix] [--pause] [--output PATH]
  help
Приклади:
  kupyna-lab.exe hash --bits 256 --text abc
  kupyna-lab.exe trace --bits 512 --hex "00 01 FF" --matrix --pause
  kupyna-lab.exe trace --bits 256 --text abc --matrix --output trace.txt
  kupyna-lab.exe hash --bits 256 --hex FF --message-bits 1
Порожнє повідомлення: --hex "". Для PowerShell 5: --file empty.bin.
Файли читаються як байти. Текст — UTF-8. HEX допускає пробіли, без 0x.
Траса: до 1024 байтів вхідних даних. Файл для hash: до 16 MiB.
)";
}

int command_line(const std::vector<std::string>& args) {
    if (args.size() == 1) return menu();
    const auto& command = args[1];
    if (command == "help" || command == "--help" || command == "-h") { help(); return 0; }
    if (command == "explain" || command == "selftest") {
        if (args.size() != 2) throw std::invalid_argument("Ця команда не приймає додаткових аргументів.");
        if (command == "explain") { explain(std::cout); return 0; }
        return run_self_tests(std::cout) ? 0 : 1;
    }
    if (command != "hash" && command != "trace") throw std::invalid_argument("Невідома команда. Використайте help.");
    Input input;
    int bits = 256;
    bool has_input = false, pause = false, matrix = false, has_bits = false, has_message_bits = false;
    std::string output;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const auto& option = args[i];
        if (option == "--pause") { pause = true; continue; }
        if (option == "--matrix") { matrix = true; continue; }
        if (option != "--bits" && option != "--text" && option != "--hex" && option != "--file"
            && option != "--message-bits" && option != "--output")
            throw std::invalid_argument("Невідомий параметр: " + option);
        if (i + 1 == args.size()) throw std::invalid_argument("Немає значення після " + option);
        const auto& value = args[++i];
        if (option == "--bits") {
            if (has_bits) throw std::invalid_argument("--bits задано двічі.");
            const auto n = number(value, "--bits");
            if (n != 256 && n != 512) throw std::invalid_argument("--bits: лише 256 або 512.");
            bits = static_cast<int>(n); has_bits = true;
        } else if (option == "--message-bits") {
            if (has_message_bits) throw std::invalid_argument("--message-bits задано двічі.");
            input.bits = number(value, "--message-bits"); has_message_bits = true;
        } else if (option == "--output") {
            if (!output.empty()) throw std::invalid_argument("--output задано двічі.");
            if (value.empty()) throw std::invalid_argument("Порожній шлях --output.");
            output = value;
        } else {
            if (has_input) throw std::invalid_argument("Оберіть одне джерело: --text, --hex або --file.");
            if (option == "--text") { input.bytes.assign(value.begin(), value.end()); input.label = "текст UTF-8"; }
            if (option == "--hex") { input.bytes = parse_hex(value); input.label = "HEX"; }
            if (option == "--file") {
                input.bytes = read_file(value); input.label = "файл: " + value; input.source_file = value;
            }
            has_input = true;
        }
    }
    if (!has_input) throw std::invalid_argument("Потрібні --text, --hex або --file.");
    if (command == "hash" && (pause || matrix || !output.empty()))
        throw std::invalid_argument("--pause, --matrix, --output доступні для trace.");
    if (pause && !output.empty()) throw std::invalid_argument("Для --output використайте trace без --pause.");
    if (output.empty()) calculate(std::cout, input, bits, command == "trace", pause, matrix);
    else {
        // Перевіряємо параметри до створення файла, щоб помилка не залишала порожній звіт.
        validate_trace_destination(input, bits, output);
        std::ofstream file(std::filesystem::u8path(output), std::ios::binary);
        if (!file) throw std::runtime_error("Не вдалося створити файл: " + output);
        const auto digest = calculate(file, input, bits, true, false, matrix);
        file.flush();
        if (!file) throw std::runtime_error("Помилка запису траси.");
        std::cout << "Купина-" << bits << ": " << hex(digest) << "\nТрасу збережено: " << output << '\n';
    }
    return 0;
}

int run(const std::vector<std::string>& args) {
    try { return command_line(args); }
    catch (const EndOfInput&) { std::cerr << "Ввід завершився під час покрокового перегляду.\n"; return 2; }
    catch (const Cancelled&) { std::cout << "Перегляд завершено користувачем.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "Помилка: " << error.what() << '\n'; return 2; }
}
} // namespace

#ifdef _WIN32
// wmain зберігає кирилицю в тексті та шляхах незалежно від системної code page.
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string value(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), size, nullptr, nullptr);
        value.pop_back(); args.push_back(std::move(value));
    }
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
