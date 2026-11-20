#pragma once
#include "media_backend.h"
#include <memory>
#include <utility>
namespace ca_zt30 {
// Base order is part of the lifetime contract: MediaBackend (and all SDK
// users) are destroyed before the resources owning the SDK and packet memory.
template<class Context> class MediaResources {
protected:
    std::unique_ptr<Context> context;
    explicit MediaResources(std::unique_ptr<Context> c):context(std::move(c)) {}
};
template<class Context> class OwnedMediaBackend : private MediaResources<Context>, public MediaBackend {
protected:
    using MediaResources<Context>::context;
public:
    explicit OwnedMediaBackend(std::unique_ptr<Context> c):MediaResources<Context>(std::move(c)),
        MediaBackend(context->api,context->encoder,context->memory) {}
};
}
