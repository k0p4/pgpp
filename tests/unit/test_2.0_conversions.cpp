// UT-CONV-001 through UT-CONV-009: Type conversion tests
// No database required — tests Internal::Details::convertPQValue<T>

#include <pgpp/pgpp_connection.h>
#include <gtest/gtest.h>

using namespace Internal::Details;

// ── UT-CONV-001: string ─────────────────────────────────────────────────────

TEST(TypeConversions, StringReturnsRawValue)
{
    EXPECT_EQ(convertPQValue<std::string>("hello"), "hello");
    EXPECT_EQ(convertPQValue<std::string>(""), "");
    EXPECT_EQ(convertPQValue<std::string>("with spaces and 123"), "with spaces and 123");
}

// ── UT-CONV-002: int ────────────────────────────────────────────────────────

TEST(TypeConversions, IntParsesCorrectly)
{
    EXPECT_EQ(convertPQValue<int>("42"), 42);
    EXPECT_EQ(convertPQValue<int>("0"), 0);
    EXPECT_EQ(convertPQValue<int>("-1"), -1);
    EXPECT_EQ(convertPQValue<int>("2147483647"), 2147483647);
}

// ── UT-CONV-003: int16_t ────────────────────────────────────────────────────

TEST(TypeConversions, Int16ParsesCorrectly)
{
    EXPECT_EQ(convertPQValue<int16_t>("123"), static_cast<int16_t>(123));
    EXPECT_EQ(convertPQValue<int16_t>("-32768"), static_cast<int16_t>(-32768));
    EXPECT_EQ(convertPQValue<int16_t>("32767"), static_cast<int16_t>(32767));
    EXPECT_EQ(convertPQValue<int16_t>("0"), static_cast<int16_t>(0));
}

// ── UT-CONV-004: int64_t ────────────────────────────────────────────────────

TEST(TypeConversions, Int64ParsesCorrectly)
{
    EXPECT_EQ(convertPQValue<int64_t>("9999999999"), 9999999999LL);
    EXPECT_EQ(convertPQValue<int64_t>("0"), 0LL);
    EXPECT_EQ(convertPQValue<int64_t>("-9223372036854775807"), -9223372036854775807LL);
}

// ── UT-CONV-005: uint32_t ───────────────────────────────────────────────────

TEST(TypeConversions, Uint32ParsesCorrectly)
{
    EXPECT_EQ(convertPQValue<uint32_t>("4000000000"), 4000000000U);
    EXPECT_EQ(convertPQValue<uint32_t>("0"), 0U);
    EXPECT_EQ(convertPQValue<uint32_t>("4294967295"), 4294967295U);
}

// ── UT-CONV-006: double ─────────────────────────────────────────────────────

TEST(TypeConversions, DoubleParsesCorrectly)
{
    EXPECT_DOUBLE_EQ(convertPQValue<double>("3.14159"), 3.14159);
    EXPECT_DOUBLE_EQ(convertPQValue<double>("0.0"), 0.0);
    EXPECT_DOUBLE_EQ(convertPQValue<double>("-1.5"), -1.5);
}

// ── UT-CONV-007: float ──────────────────────────────────────────────────────

TEST(TypeConversions, FloatParsesCorrectly)
{
    EXPECT_FLOAT_EQ(convertPQValue<float>("2.5"), 2.5f);
    EXPECT_FLOAT_EQ(convertPQValue<float>("0.0"), 0.0f);
    EXPECT_FLOAT_EQ(convertPQValue<float>("-100.25"), -100.25f);
}

// ── UT-CONV-008: bool true ──────────────────────────────────────────────────

TEST(TypeConversions, BoolTrueValues)
{
    EXPECT_TRUE(convertPQValue<bool>("t"));
    EXPECT_TRUE(convertPQValue<bool>("T"));
    EXPECT_TRUE(convertPQValue<bool>("1"));
}

// ── UT-CONV-009: bool false ─────────────────────────────────────────────────

TEST(TypeConversions, BoolFalseValues)
{
    EXPECT_FALSE(convertPQValue<bool>("f"));
    EXPECT_FALSE(convertPQValue<bool>("F"));
    EXPECT_FALSE(convertPQValue<bool>("0"));
}

// ── Edge cases: malformed input ─────────────────────────────────────────────

TEST(TypeConversions, IntThrowsOnEmptyString)
{
    EXPECT_THROW(convertPQValue<int>(""), std::invalid_argument);
}

TEST(TypeConversions, IntThrowsOnNonNumeric)
{
    EXPECT_THROW(convertPQValue<int>("abc"), std::invalid_argument);
}

TEST(TypeConversions, DoubleThrowsOnEmptyString)
{
    EXPECT_THROW(convertPQValue<double>(""), std::invalid_argument);
}

TEST(TypeConversions, Uint32ParsesMaxValue)
{
    EXPECT_EQ(convertPQValue<uint32_t>("4294967295"), 4294967295U);
}

TEST(TypeConversions, Int64ParsesMinValue)
{
    // INT64_MIN + 1 to avoid literal issues
    EXPECT_EQ(convertPQValue<int64_t>("-9223372036854775807"), -9223372036854775807LL);
}

// ── String edge cases ───────────────────────────────────────────────────────

TEST(TypeConversions, StringUtf8Multibyte)
{
    const char* cyrillic = "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"; // Привет
    EXPECT_EQ(convertPQValue<std::string>(cyrillic), std::string(cyrillic));
}

// ── Integer overflow/underflow ──────────────────────────────────────────────

TEST(TypeConversions, IntOverflowThrows)
{
    // INT_MAX + 1 = 2147483648
    EXPECT_THROW(convertPQValue<int>("2147483648"), std::out_of_range);
}

TEST(TypeConversions, IntUnderflowThrows)
{
    // INT_MIN - 1 = -2147483649
    EXPECT_THROW(convertPQValue<int>("-2147483649"), std::out_of_range);
}

TEST(TypeConversions, Int16Truncation)
{
    // stoi("40000") succeeds, but static_cast<int16_t> truncates silently
    // This documents current behavior — no range check in the conversion
    int16_t result = convertPQValue<int16_t>("40000");
    EXPECT_NE(result, 40000); // truncated
}

TEST(TypeConversions, Int64MaxValue)
{
    EXPECT_EQ(convertPQValue<int64_t>("9223372036854775807"), INT64_MAX);
}

// ── Float edge cases ────────────────────────────────────────────────────────

TEST(TypeConversions, FloatScientificNotation)
{
    EXPECT_NEAR(convertPQValue<double>("1.5e-10"), 1.5e-10, 1e-20);
    EXPECT_NEAR(convertPQValue<float>("2.5e3"), 2500.0f, 0.1f);
}

TEST(TypeConversions, DoubleInfinity)
{
    double posInf = convertPQValue<double>("inf");
    double negInf = convertPQValue<double>("-inf");
    EXPECT_TRUE(std::isinf(posInf));
    EXPECT_GT(posInf, 0.0);
    EXPECT_TRUE(std::isinf(negInf));
    EXPECT_LT(negInf, 0.0);
}

TEST(TypeConversions, DoubleNaN)
{
    double nan = convertPQValue<double>("nan");
    EXPECT_TRUE(std::isnan(nan));
}

// ── Bool edge cases ─────────────────────────────────────────────────────────

TEST(TypeConversions, BoolFirstCharLogic)
{
    // Implementation: raw[0] == 't' || raw[0] == 'T' || raw[0] == '1'
    // "true" → 't' → true
    EXPECT_TRUE(convertPQValue<bool>("true"));
    // "false" → 'f' → false
    EXPECT_FALSE(convertPQValue<bool>("false"));
    // "yes" → 'y' → false (not recognized)
    EXPECT_FALSE(convertPQValue<bool>("yes"));
    // "no" → 'n' → false
    EXPECT_FALSE(convertPQValue<bool>("no"));
}
