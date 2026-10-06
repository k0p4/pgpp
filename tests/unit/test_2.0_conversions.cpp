// UT-CONV-001 through UT-CONV-022: binary result decoding
// No database required — tests Internal::Details::decodeBinary<T>.
//
// Results arrive from libpq in binary format (REQ-PGPP-062): each cell is the
// PostgreSQL wire encoding of its column type. decodeBinary<T>(oid, bytes)
// returns std::expected<T, ConversionError>; pgpp never throws (REQ-PGPP-061).

#include <pgpp/pgpp_connection.h>
#include <gtest/gtest.h>

#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace Internal::Details;

namespace {

// PostgreSQL sends integers and IEEE floats in network byte order.
template<typename U>
std::string bigEndian(U value)
{
    std::string bytes(sizeof(U), '\0');
    for (size_t i = 0; i < sizeof(U); ++i) {
        bytes[sizeof(U) - 1 - i] = static_cast<char>(value & 0xFF);
        value >>= 8;
    }
    return bytes;
}

std::string int2Cell(int16_t v)  { return bigEndian(static_cast<uint16_t>(v)); }
std::string int4Cell(int32_t v)  { return bigEndian(static_cast<uint32_t>(v)); }
std::string int8Cell(int64_t v)  { return bigEndian(static_cast<uint64_t>(v)); }
std::string oidCell(uint32_t v)  { return bigEndian(v); }
std::string float4Cell(float v)  { return bigEndian(std::bit_cast<uint32_t>(v)); }
std::string float8Cell(double v) { return bigEndian(std::bit_cast<uint64_t>(v)); }

} // namespace

// ── UT-CONV-001..003: std::string ───────────────────────────────────────────

TEST(BinaryDecoding, TextualColumnsIntoString)
{
    EXPECT_EQ(decodeBinary<std::string>(pg::TEXT,    "hello"), "hello");
    EXPECT_EQ(decodeBinary<std::string>(pg::VARCHAR, "with spaces and 123"), "with spaces and 123");
    EXPECT_EQ(decodeBinary<std::string>(pg::BPCHAR,  "abc  "), "abc  ");
    EXPECT_EQ(decodeBinary<std::string>(pg::NAME,    "pgpp_test_table"), "pgpp_test_table");
    EXPECT_EQ(decodeBinary<std::string>(pg::UNKNOWN, "literal"), "literal");   // SELECT 'literal'
    EXPECT_EQ(decodeBinary<std::string>(pg::JSON,    "{\"a\":1}"), "{\"a\":1}");
    EXPECT_EQ(decodeBinary<std::string>(pg::TEXT,    ""), "");
}

TEST(BinaryDecoding, JsonbStripsVersionByte)
{
    EXPECT_EQ(decodeBinary<std::string>(pg::JSONB, std::string("\x01{\"a\":1}", 8)), "{\"a\":1}");
    EXPECT_EQ(decodeBinary<std::string>(pg::JSONB, "").error(), ConversionError::Malformed);
    EXPECT_EQ(decodeBinary<std::string>(pg::JSONB, "\x02{}").error(), ConversionError::Malformed);
}

TEST(BinaryDecoding, NonTextualColumnIntoStringIsTypeMismatch)
{
    // int, timestamp, uuid, numeric, ...: cast to ::text in the SQL instead.
    EXPECT_EQ(decodeBinary<std::string>(pg::INT4,      int4Cell(42)).error(), ConversionError::TypeMismatch);
    EXPECT_EQ(decodeBinary<std::string>(pg::TIMESTAMP, int8Cell(0)).error(),  ConversionError::TypeMismatch);
    EXPECT_EQ(decodeBinary<std::string>(pg::BOOL,      "\x01").error(),       ConversionError::TypeMismatch);
}

// ── UT-CONV-004..012: integers ──────────────────────────────────────────────

TEST(BinaryDecoding, Int4Decodes)
{
    EXPECT_EQ(decodeBinary<int>(pg::INT4, int4Cell(42)), 42);
    EXPECT_EQ(decodeBinary<int>(pg::INT4, int4Cell(0)), 0);
    EXPECT_EQ(decodeBinary<int>(pg::INT4, int4Cell(-1)), -1);
    EXPECT_EQ(decodeBinary<int>(pg::INT4, int4Cell(INT32_MAX)), INT32_MAX);
    EXPECT_EQ(decodeBinary<int>(pg::INT4, int4Cell(INT32_MIN)), INT32_MIN);
}

TEST(BinaryDecoding, Int2Decodes)
{
    EXPECT_EQ(decodeBinary<int16_t>(pg::INT2, int2Cell(123)), static_cast<int16_t>(123));
    EXPECT_EQ(decodeBinary<int16_t>(pg::INT2, int2Cell(-32768)), static_cast<int16_t>(-32768));
    EXPECT_EQ(decodeBinary<int16_t>(pg::INT2, int2Cell(32767)), static_cast<int16_t>(32767));
}

TEST(BinaryDecoding, Int8Decodes)
{
    EXPECT_EQ(decodeBinary<int64_t>(pg::INT8, int8Cell(9999999999LL)), 9999999999LL);
    EXPECT_EQ(decodeBinary<int64_t>(pg::INT8, int8Cell(INT64_MAX)), INT64_MAX);
    EXPECT_EQ(decodeBinary<int64_t>(pg::INT8, int8Cell(INT64_MIN)), INT64_MIN);
}

TEST(BinaryDecoding, NarrowerColumnWidens)
{
    EXPECT_EQ(decodeBinary<int>(pg::INT2, int2Cell(-7)), -7);
    EXPECT_EQ(decodeBinary<int64_t>(pg::INT2, int2Cell(7)), 7LL);
    EXPECT_EQ(decodeBinary<int64_t>(pg::INT4, int4Cell(-123456)), -123456LL);
}

TEST(BinaryDecoding, Int8IntoIntIsRangeChecked)
{
    // SELECT COUNT(*) is int8; reading it into int is fine while it fits.
    EXPECT_EQ(decodeBinary<int>(pg::INT8, int8Cell(2147483647LL)), 2147483647);
    EXPECT_EQ(decodeBinary<int>(pg::INT8, int8Cell(3000000000LL)).error(),  ConversionError::OutOfRange);
    EXPECT_EQ(decodeBinary<int>(pg::INT8, int8Cell(-3000000000LL)).error(), ConversionError::OutOfRange);
}

TEST(BinaryDecoding, Int16RejectsOutOfRange)
{
    // 40000 into int16_t used to wrap to -25536.
    EXPECT_EQ(decodeBinary<int16_t>(pg::INT4, int4Cell(40000)).error(),  ConversionError::OutOfRange);
    EXPECT_EQ(decodeBinary<int16_t>(pg::INT4, int4Cell(-40000)).error(), ConversionError::OutOfRange);
    EXPECT_EQ(decodeBinary<int16_t>(pg::INT4, int4Cell(32767)), static_cast<int16_t>(32767));
}

TEST(BinaryDecoding, Uint32FromOidAndIntegerColumns)
{
    EXPECT_EQ(decodeBinary<uint32_t>(pg::OID,  oidCell(4294967295U)), 4294967295U);
    EXPECT_EQ(decodeBinary<uint32_t>(pg::INT4, int4Cell(4000000)), 4000000U);
    EXPECT_EQ(decodeBinary<uint32_t>(pg::INT8, int8Cell(4294967295LL)), 4294967295U);
    EXPECT_EQ(decodeBinary<uint32_t>(pg::INT4, int4Cell(-1)).error(),          ConversionError::OutOfRange); // never 4294967295
    EXPECT_EQ(decodeBinary<uint32_t>(pg::INT8, int8Cell(5000000000LL)).error(), ConversionError::OutOfRange); // never truncated
}

TEST(BinaryDecoding, TextIntoNumberIsTypeMismatch)
{
    EXPECT_EQ(decodeBinary<int>(pg::TEXT, "42").error(),      ConversionError::TypeMismatch);
    EXPECT_EQ(decodeBinary<int64_t>(pg::TEXT, "42").error(),  ConversionError::TypeMismatch);
    EXPECT_EQ(decodeBinary<double>(pg::TEXT, "1.5").error(),  ConversionError::TypeMismatch);
    EXPECT_EQ(decodeBinary<int>(pg::UNKNOWN, "42").error(),   ConversionError::TypeMismatch);
}

TEST(BinaryDecoding, NumericHasNoDecoder)
{
    // Cast to ::int8 / ::float8 in SQL; numeric's binary form is not decoded.
    const std::string numeric42("\x00\x01\x00\x00\x00\x00\x00\x00\x00\x2a", 10);   // ndigits=1 weight=0 sign=0 dscale=0 digit=42
    EXPECT_EQ(decodeBinary<int64_t>(pg::NUMERIC, numeric42).error(), ConversionError::TypeMismatch);
    EXPECT_EQ(decodeBinary<double>(pg::NUMERIC,  numeric42).error(), ConversionError::TypeMismatch);
}

TEST(BinaryDecoding, WrongCellSizeIsMalformed)
{
    EXPECT_EQ(decodeBinary<int>(pg::INT4, std::string("\x00\x00\x2a", 3)).error(), ConversionError::Malformed);
    EXPECT_EQ(decodeBinary<int64_t>(pg::INT8, int4Cell(1)).error(),      ConversionError::Malformed);
    EXPECT_EQ(decodeBinary<double>(pg::FLOAT8, float4Cell(1.0f)).error(), ConversionError::Malformed);
    EXPECT_EQ(decodeBinary<bool>(pg::BOOL, std::string("\x01\x00", 2)).error(), ConversionError::Malformed);
}

// ── UT-CONV-013..017: floating point ────────────────────────────────────────

TEST(BinaryDecoding, Float8Decodes)
{
    EXPECT_DOUBLE_EQ(decodeBinary<double>(pg::FLOAT8, float8Cell(3.14159)).value_or(0.0), 3.14159);
    EXPECT_DOUBLE_EQ(decodeBinary<double>(pg::FLOAT8, float8Cell(0.0)).value_or(-1.0), 0.0);
    EXPECT_DOUBLE_EQ(decodeBinary<double>(pg::FLOAT8, float8Cell(-1.5)).value_or(0.0), -1.5);
    EXPECT_DOUBLE_EQ(decodeBinary<double>(pg::FLOAT8, float8Cell(1.5e-10)).value_or(0.0), 1.5e-10);
}

TEST(BinaryDecoding, Float4Decodes)
{
    EXPECT_FLOAT_EQ(decodeBinary<float>(pg::FLOAT4, float4Cell(2.5f)).value_or(0.0f), 2.5f);
    EXPECT_FLOAT_EQ(decodeBinary<float>(pg::FLOAT4, float4Cell(-100.25f)).value_or(0.0f), -100.25f);
    EXPECT_DOUBLE_EQ(decodeBinary<double>(pg::FLOAT4, float4Cell(2.5f)).value_or(0.0), 2.5);   // float4 into double
}

TEST(BinaryDecoding, Float8IntoFloatIsRangeChecked)
{
    EXPECT_FLOAT_EQ(decodeBinary<float>(pg::FLOAT8, float8Cell(2500.0)).value_or(0.0f), 2500.0f);
    EXPECT_EQ(decodeBinary<float>(pg::FLOAT8, float8Cell(1e39)).error(),  ConversionError::OutOfRange);
    EXPECT_EQ(decodeBinary<float>(pg::FLOAT8, float8Cell(-1e39)).error(), ConversionError::OutOfRange);
}

TEST(BinaryDecoding, IntegerColumnIntoFloatingPoint)
{
    EXPECT_DOUBLE_EQ(decodeBinary<double>(pg::INT4, int4Cell(42)).value_or(0.0), 42.0);
    EXPECT_DOUBLE_EQ(decodeBinary<double>(pg::INT8, int8Cell(1234567LL)).value_or(0.0), 1234567.0);
    EXPECT_FLOAT_EQ(decodeBinary<float>(pg::INT2, int2Cell(-3)).value_or(0.0f), -3.0f);
}

TEST(BinaryDecoding, DoubleInfinityAndNaN)
{
    auto posInf = decodeBinary<double>(pg::FLOAT8, float8Cell(INFINITY));
    auto negInf = decodeBinary<double>(pg::FLOAT8, float8Cell(-INFINITY));
    auto nan    = decodeBinary<double>(pg::FLOAT8, float8Cell(NAN));
    ASSERT_TRUE(posInf.has_value());
    ASSERT_TRUE(negInf.has_value());
    ASSERT_TRUE(nan.has_value());
    EXPECT_TRUE(std::isinf(*posInf));
    EXPECT_GT(*posInf, 0.0);
    EXPECT_TRUE(std::isinf(*negInf));
    EXPECT_LT(*negInf, 0.0);
    EXPECT_TRUE(std::isnan(*nan));
    // Infinity is representable as float too, so it is not "out of range".
    EXPECT_TRUE(std::isinf(decodeBinary<float>(pg::FLOAT8, float8Cell(INFINITY)).value_or(0.0f)));
}

// ── UT-CONV-018..020: bool, UTF-8, OID catalog ──────────────────────────────

TEST(BinaryDecoding, BoolDecodes)
{
    EXPECT_EQ(decodeBinary<bool>(pg::BOOL, std::string("\x01", 1)), true);
    EXPECT_EQ(decodeBinary<bool>(pg::BOOL, std::string("\x00", 1)), false);
    EXPECT_EQ(decodeBinary<bool>(pg::INT4, int4Cell(1)).error(), ConversionError::TypeMismatch);
    EXPECT_EQ(decodeBinary<bool>(pg::TEXT, "t").error(),         ConversionError::TypeMismatch);
}

TEST(BinaryDecoding, StringUtf8Multibyte)
{
    const char* cyrillic = "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"; // Привет
    EXPECT_EQ(decodeBinary<std::string>(pg::TEXT, cyrillic), std::string(cyrillic));
}

TEST(BinaryDecoding, TextualTypeSet)
{
    for (uint32_t oid : {pg::CHAR, pg::NAME, pg::TEXT, pg::JSON, pg::XML, pg::UNKNOWN, pg::BPCHAR, pg::VARCHAR, pg::CSTRING, pg::JSONB}) {
        EXPECT_TRUE(isTextualType(oid)) << oid;
    }
    for (uint32_t oid : {pg::BOOL, pg::BYTEA, pg::INT2, pg::INT4, pg::INT8, pg::OID, pg::FLOAT4, pg::FLOAT8,
                         pg::DATE, pg::TIME, pg::TIMESTAMP, pg::TIMESTAMPTZ, pg::TIMETZ, pg::NUMERIC}) {
        EXPECT_FALSE(isTextualType(oid)) << oid;
    }
}

// ── UT-CONV-023: std::optional<T> columns (REQ-PGPP-075) ────────────────────

// A non-NULL cell decodes exactly as T and is wrapped; every decoding error
// is passed through unchanged. (NULL never reaches the decoder: fillPQValue
// leaves the optional empty.)
TEST(BinaryDecoding, OptionalWrapsTheInnerDecoder)
{
    EXPECT_EQ(decodeBinary<std::optional<int>>(pg::INT4, int4Cell(42)).value(), std::optional<int>(42));
    EXPECT_EQ(decodeBinary<std::optional<std::string>>(pg::TEXT, "x").value(), std::optional<std::string>("x"));
    EXPECT_EQ(decodeBinary<std::optional<bool>>(pg::BOOL, std::string("\x00", 1)).value(), std::optional<bool>(false));
    EXPECT_DOUBLE_EQ(decodeBinary<std::optional<double>>(pg::FLOAT8, float8Cell(1.5)).value().value(), 1.5);

    EXPECT_EQ(decodeBinary<std::optional<int16_t>>(pg::INT4, int4Cell(40000)).error(), ConversionError::OutOfRange);
    EXPECT_EQ(decodeBinary<std::optional<int>>(pg::TEXT, "42").error(),               ConversionError::TypeMismatch);
    EXPECT_EQ(decodeBinary<std::optional<int>>(pg::INT4, std::string("\x00", 1)).error(), ConversionError::Malformed);
}

// ── UT-CONV-024: parameters (REQ-PGPP-074/075) ──────────────────────────────

TEST(BinaryDecoding, ParameterPointersAndEmbeddedNul)
{
    const std::string plain("abc");
    const std::optional<std::string> absent;
    const std::optional<std::string> present("xyz");
    EXPECT_STREQ(paramValue(plain), "abc");
    EXPECT_EQ(paramValue(absent), nullptr);        // SQL NULL
    EXPECT_STREQ(paramValue(present), "xyz");

    std::string withNul("a");
    withNul.push_back('\0');
    withNul += "b";
    EXPECT_FALSE(paramHasEmbeddedNul(plain));
    EXPECT_FALSE(paramHasEmbeddedNul(absent));
    EXPECT_FALSE(paramHasEmbeddedNul(present));
    EXPECT_FALSE(paramHasEmbeddedNul(std::string()));
    EXPECT_TRUE(paramHasEmbeddedNul(withNul));
    EXPECT_TRUE(paramHasEmbeddedNul(std::optional<std::string>(withNul)));
}

// ── UT-CONV-022: zero result columns ────────────────────────────────────────

// A row tuple with no elements is a valid (if unusual) result type: it must
// instantiate without a zero-length array (ill-formed, MSVC C2466) and behave
// like any other result overload. The connection is never opened, so the call
// fails cleanly instead of touching a server.
TEST(BinaryDecoding, EmptyRowTupleInstantiates)
{
    PgppConnection conn;
    std::vector<std::tuple<>> rows;
    EXPECT_FALSE(conn.execPrepared("no_such_statement", rows));
    EXPECT_TRUE(rows.empty());
}
