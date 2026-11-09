#pragma once

#include "ieg/acquisition/acquisition_source.hpp"

#include <gmock/gmock.h>

#include <string>
#include <utility>
#include <vector>

namespace ieg::acquisition::testing {

class MockAcquisitionSource : public IAcquisitionSource
{
public:
    explicit MockAcquisitionSource(std::string id) : id_(std::move(id)) {}

    [[nodiscard]] const std::string& id() const noexcept override { return id_; }
    MOCK_METHOD(void, start, (std::vector<Item> items, Callbacks callbacks), (override));
    // gmock's generated body can throw on expectation failures; that only aborts a test run.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    MOCK_METHOD(void, stop, (), (noexcept, override));

private:
    std::string id_;
};

} // namespace ieg::acquisition::testing
