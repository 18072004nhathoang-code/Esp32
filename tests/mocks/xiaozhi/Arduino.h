#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "freertos/semphr.h"

class String
{
public:
    String() = default;
    String(const char *value) : value_(value ? value : "") {}
    String(unsigned value) : value_(std::to_string(value)) {}
    size_t length() const { return value_.size(); }
    const char *c_str() const { return value_.c_str(); }
    char operator[](size_t index) const { return value_[index]; }
    int indexOf(char value) const { return index(value_.find(value)); }
    int lastIndexOf(char value) const { return index(value_.rfind(value)); }
    String substring(size_t start) const { return String(value_.substr(start).c_str()); }
    String substring(size_t start, size_t end) const
    {
        return String(value_.substr(start, end - start).c_str());
    }
    long toInt() const { return std::strtol(c_str(), nullptr, 10); }
    String &operator+=(const String &other) { value_ += other.value_; return *this; }
    bool operator==(const char *other) const { return value_ == other; }
    friend String operator+(const String &left, const String &right)
    {
        return String((left.value_ + right.value_).c_str());
    }
private:
    static int index(size_t value) { return value == std::string::npos ? -1 : static_cast<int>(value); }
    std::string value_;
};

uint32_t millis();
inline size_t strlcpy(char *out, const char *value, size_t capacity)
{
    const size_t length = std::strlen(value);
    if (capacity)
    {
        const size_t count = std::min(length, capacity - 1);
        std::memcpy(out, value, count);
        out[count] = '\0';
    }
    return length;
}
#define log_e(...) do { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
