#include <cutefish/store/error.h>
#include <cutefish/store/result.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace cutefish::store;

TEST(Error, CarriesCodeAndMessage)
{
    const Error e = Error::notFound("no such application");
    EXPECT_EQ(e.code(), ErrorCode::NotFound);
    EXPECT_EQ(e.backendFailure(), BackendFailure::Unspecified);
    EXPECT_EQ(e.message(), "no such application");
}

TEST(Error, BackendErrorsCarryTheirReason)
{
    const Error e = Error::backend(BackendFailure::AuthDenied, "polkit refused");
    EXPECT_EQ(e.code(), ErrorCode::Backend);
    EXPECT_EQ(e.backendFailure(), BackendFailure::AuthDenied);
}

TEST(Error, EveryCategoryHasItsFactory)
{
    EXPECT_EQ(Error::invalidArgument("x").code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(Error::notSupported("x").code(), ErrorCode::NotSupported);
    EXPECT_EQ(Error::conflict("x").code(), ErrorCode::Conflict);
    EXPECT_EQ(Error::cancelled().code(), ErrorCode::Cancelled);
    EXPECT_EQ(Error::internal("x").code(), ErrorCode::Internal);
}

TEST(Error, NamesAreStable)
{
    EXPECT_EQ(toString(ErrorCode::InvalidArgument), "invalid-argument");
    EXPECT_EQ(toString(ErrorCode::Backend), "backend");
    EXPECT_EQ(toString(BackendFailure::AuthCancelled), "auth-cancelled");
    EXPECT_EQ(toString(BackendFailure::Unavailable), "backend-unavailable");
    EXPECT_EQ(toString(BackendFailure::PackageConflict), "conflict");
}

TEST(Error, ComparesByValue)
{
    EXPECT_EQ(Error::notFound("a"), Error::notFound("a"));
    EXPECT_NE(Error::notFound("a"), Error::notFound("b"));
    EXPECT_NE(Error::backend(BackendFailure::Network, "a"), Error::backend(BackendFailure::Locked, "a"));
}

TEST(Result, HoldsAValue)
{
    Result<int> r = 42;
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value(), 42);
    EXPECT_EQ(*r, 42);
    EXPECT_EQ(r.valueOr(0), 42);
}

TEST(Result, HoldsAnError)
{
    Result<int> r = Error::invalidArgument("negative");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(r.valueOr(-1), -1);
}

TEST(Result, MovesItsValueOut)
{
    Result<std::unique_ptr<int>> r = std::make_unique<int>(7);
    ASSERT_TRUE(r);
    std::unique_ptr<int> p = std::move(r).value();
    EXPECT_EQ(*p, 7);
}

TEST(Result, StringValueAndArrow)
{
    Result<std::string> r = std::string("abc");
    EXPECT_EQ(r->size(), 3u);
}

TEST(Result, VoidSuccessAndFailure)
{
    Result<void> ok = Result<void>::success();
    EXPECT_TRUE(ok);
    Result<void> failed = Error::cancelled();
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code(), ErrorCode::Cancelled);
}
