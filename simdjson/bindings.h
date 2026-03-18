#include <napi.h>
#include "src/simdjson.h"

namespace simdjsonnode {
  using namespace simdjson;

  bool isValid(std::string p);
  Napi::Boolean IsValidWrapped(const Napi::CallbackInfo& info);

  Napi::Object parse(Napi::Env env, std::string p);
  Napi::Value makeJSONObject(Napi::Env env, dom::element element);
  Napi::Value ParseWrapped(const Napi::CallbackInfo& info);

  Napi::Object LazyParseWrapped(const Napi::CallbackInfo& info);
  Napi::Value ValueForKeyPathWrapped(const Napi::CallbackInfo& info);
  Napi::Value findKeyPath(Napi::Env env, std::vector<std::string> subpaths, dom::element pjh);

  // FindChunkBoundaries: navigate to a dot-path inside a JSON Buffer, locate the array there,
  // and return [[start,end], ...] byte-offset pairs that chunk it into pieces < maxChunkBytes.
  // Each [start,end] slice of the original Buffer is a valid JSON array body (no outer [ ]).
  // Caller wraps: JSON.parse('[' + buf.toString('utf8', start, end) + ']')
  Napi::Value FindChunkBoundariesWrapped(const Napi::CallbackInfo& info);

  Napi::Object Init(Napi::Env env, Napi::Object exports);
}
