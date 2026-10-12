#pragma once

#include "kupyna.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace kupyna::tests {

struct TestVector {
    std::string name;
    int digest_bits;
    std::size_t message_bits;
    Bytes message;
    std::string expected_hex;
};

// Все 16 эталонных примеров Купины-256/512 из приложенного Kupyna-reference/main.c.
// Для неполных байтов значимы старшие message_bits % 8 бит.
const std::vector<TestVector>& reference_vectors();

// Возвращает false при любой ошибке; подробности направляет в output.
bool run_self_tests(std::ostream& output);

} // namespace kupyna::tests

// Удобный вызов из консольного entry point.
bool run_self_tests(std::ostream& output);
