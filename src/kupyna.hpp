#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Учебная реализация ДСТУ 7564:2014: Купина — хеш-функция, а не шифр.
namespace kupyna {

using Bytes = std::vector<std::uint8_t>;

struct Params {
    int hash_bits;                  // Размер результата: 256 или 512 бит.
    int state_bits;                 // Размер состояния: 512 или 1024 бита.
    int columns;                    // Состояние всегда имеет 8 строк.
    int rounds;                     // 10 или 14 раундов каждой перестановки.
    std::size_t block_bytes;
    std::size_t output_bytes;
};

enum class Operation {
    Input,
    Padding,
    Initialize,
    Block,
    XorInput,
    PermutationInput,
    AddRoundConstant,
    SubBytes,
    ShiftRows,
    MixColumns,
    Compression,
    OutputXor,
    Truncate
};

struct Step {
    std::size_t sequence = 0;        // Номер события, начиная с 1.
    std::size_t block_index = 0;     // Блоки с 1; подготовка и выход — 0.
    std::string branch;             // "P", "Q", "output" или пустая строка.
    std::optional<int> round;       // Номер раунда из стандарта: 0 .. R-1.
    Operation operation = Operation::Input;
    std::string name;               // Короткое имя операции для интерфейса.
    std::string description;        // Объяснение выполненного действия.
    Bytes state;                    // Состояние после операции.
    Bytes before;                   // Состояние до операции, если применимо.
    Bytes data;                     // Константы, блок или другие операнды.
};

// Порядок байтов состояния: state[column * 8 + row].
// Callback вызывается синхронно. Для сохранения шага скопируйте Step.
// Без callback история не создаётся. Padding содержит лишь последние 1–2 блока.
using TraceCallback = std::function<void(const Step&)>;

Params get_params(int bits);
const char* operation_name(Operation operation) noexcept;

// message_bits ограничивает вход первыми N битами, включая старшие биты
// последнего байта; неиспользованные младшие биты игнорируются.
// std::nullopt означает, что используются все байты message.
// Некорректные параметры вызывают std::invalid_argument / std::length_error.
Bytes hash(const Bytes& message, int bits,
           std::optional<std::size_t> message_bits = std::nullopt,
           TraceCallback trace = {});

} // namespace kupyna
