#pragma once

struct ReturnAddress {
    explicit ReturnAddress(const void* returnAddress) noexcept
        : returnAddress{ returnAddress }
    {
    }

    bool operator==(const ReturnAddress&) const = default;

    [[nodiscard]] const void* get() const noexcept { return returnAddress; }

private:
    const void* returnAddress;
};
