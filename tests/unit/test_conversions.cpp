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
