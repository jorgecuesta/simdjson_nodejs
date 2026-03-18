#include "bindings.h"
#include <cstring>

// ---------------------------------------------------------------------------
// JSON state-machine helpers for FindChunkBoundaries
// ---------------------------------------------------------------------------

static size_t skipWhitespace(const uint8_t* d, size_t len, size_t pos) {
  while (pos < len && (d[pos] == ' ' || d[pos] == '\n' || d[pos] == '\r' || d[pos] == '\t'))
    pos++;
  return pos;
}

// Skip a complete JSON value starting at pos. Returns position after the value.
static size_t skipValue(const uint8_t* d, size_t len, size_t pos) {
  pos = skipWhitespace(d, len, pos);
  if (pos >= len) return pos;

  if (d[pos] == '"') {
    pos++; // opening quote
    while (pos < len) {
      if (d[pos] == '\\') { pos += 2; continue; }
      if (d[pos] == '"')  { pos++; break; }
      pos++;
    }
    return pos;
  }

  if (d[pos] == '{' || d[pos] == '[') {
    uint8_t open = d[pos], close = (open == '{') ? '}' : ']';
    int depth = 1;
    bool in_str = false;
    pos++;
    while (pos < len && depth > 0) {
      if (in_str) {
        if (d[pos] == '\\') { pos++; }
        else if (d[pos] == '"') { in_str = false; }
      } else {
        if      (d[pos] == '"')   { in_str = true; }
        else if (d[pos] == open)  { depth++; }
        else if (d[pos] == close) { depth--; }
      }
      pos++;
    }
    return pos;
  }

  // number / true / false / null
  while (pos < len && d[pos] != ',' && d[pos] != '}' && d[pos] != ']' &&
         d[pos] != ' ' && d[pos] != '\n' && d[pos] != '\r' && d[pos] != '\t')
    pos++;
  return pos;
}

// Inside an object (pos right after opening '{'), find the value for `key`.
// Returns position of value start, or SIZE_MAX if not found.
static size_t findKeyInObject(const uint8_t* d, size_t len, size_t pos, const std::string& key) {
  pos = skipWhitespace(d, len, pos);
  while (pos < len && d[pos] != '}') {
    if (d[pos] != '"') break; // malformed
    pos++; // skip opening quote of key
    size_t keyStart = pos;
    while (pos < len && d[pos] != '"') {
      if (d[pos] == '\\') pos++;
      pos++;
    }
    std::string foundKey(reinterpret_cast<const char*>(d + keyStart), pos - keyStart);
    if (pos < len) pos++; // skip closing quote
    pos = skipWhitespace(d, len, pos);
    if (pos >= len || d[pos] != ':') break;
    pos++; // skip ':'
    pos = skipWhitespace(d, len, pos);

    if (foundKey == key) return pos; // found — pos is at value start

    pos = skipValue(d, len, pos);
    pos = skipWhitespace(d, len, pos);
    if (pos < len && d[pos] == ',') pos++;
    pos = skipWhitespace(d, len, pos);
  }
  return SIZE_MAX;
}

// Navigate a dot-separated path through the JSON buffer.
// Returns position of the target value, or SIZE_MAX on failure.
static size_t navigatePath(const uint8_t* d, size_t len, const std::vector<std::string>& segments) {
  size_t pos = skipWhitespace(d, len, 0);
  if (pos >= len) return SIZE_MAX;

  for (size_t i = 0; i < segments.size(); i++) {
    if (d[pos] != '{') return SIZE_MAX;
    pos++; // enter object
    pos = findKeyInObject(d, len, pos, segments[i]);
    if (pos == SIZE_MAX) return SIZE_MAX;
    if (i + 1 < segments.size()) pos = skipWhitespace(d, len, pos);
  }
  return pos;
}

// Scan the content of a JSON array (pos right after '[') and return chunk boundaries.
// Each boundary is a [start, end) byte range that is a valid JSON array body (no outer [ ]).
// Caller wraps: JSON.parse('[' + buf.toString('utf8', start, end) + ']')
static std::vector<std::pair<size_t,size_t>>
scanArrayChunks(const uint8_t* d, size_t len, size_t pos, size_t maxChunkBytes) {
  std::vector<std::pair<size_t,size_t>> chunks;

  pos = skipWhitespace(d, len, pos);
  if (pos >= len || d[pos] == ']') return chunks; // empty array

  size_t chunkStart = pos;
  size_t chunkBytes = 0;
  bool in_str = false;
  int depth = 0; // 0 = inside array at element level

  while (pos < len) {
    uint8_t c = d[pos];

    if (in_str) {
      if (c == '\\') { pos++; } // skip escaped char
      else if (c == '"') { in_str = false; }
      pos++;
      chunkBytes++;
      continue;
    }

    if (c == '"')       { in_str = true;  pos++; chunkBytes++; continue; }
    if (c == '{' || c == '[') { depth++;  pos++; chunkBytes++; continue; }
    if (c == '}' || c == ']') {
      if (depth > 0) { depth--; pos++; chunkBytes++; continue; }
      // depth == 0: closing ']' of our array — flush last chunk
      if (chunkBytes > 0) {
        // trim trailing whitespace from chunk end
        size_t end = pos;
        while (end > chunkStart && (d[end-1] == ' ' || d[end-1] == '\n' ||
               d[end-1] == '\r' || d[end-1] == '\t')) end--;
        chunks.push_back({chunkStart, end});
      }
      break;
    }

    if (c == ',' && depth == 0) {
      // Element separator — potential split point
      if (chunkBytes >= maxChunkBytes) {
        // Trim trailing whitespace
        size_t end = pos;
        while (end > chunkStart && (d[end-1] == ' ' || d[end-1] == '\n' ||
               d[end-1] == '\r' || d[end-1] == '\t')) end--;
        chunks.push_back({chunkStart, end});
        pos++; // skip ','
        pos = skipWhitespace(d, len, pos);
        chunkStart = pos;
        chunkBytes = 0;
        continue;
      }
    }

    pos++;
    chunkBytes++;
  }

  return chunks;
}

// ---------------------------------------------------------------------------

static simdjson::padded_string getInputAsPaddedString(const Napi::Value& val) {
  if (val.IsBuffer()) {
    auto buf = val.As<Napi::Buffer<uint8_t>>();
    return simdjson::padded_string(reinterpret_cast<const char*>(buf.Data()), buf.ByteLength());
  }
  std::string str = val.As<Napi::String>().Utf8Value();
  return simdjson::padded_string(str.data(), str.size());
}

bool simdjsonnode::isValid(std::string json) {
  dom::parser parser;
  return !parser.parse(json).error();
}

Napi::Boolean simdjsonnode::IsValidWrapped(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  padded_string input = getInputAsPaddedString(info[0]);
  dom::parser parser;
  Napi::Boolean returnValue = Napi::Boolean::New(env, !parser.parse(input).error());
  return returnValue;
}

Napi::Value simdjsonnode::makeJSONObject(Napi::Env env, dom::element element) {
  Napi::Value v;
  switch (element.type()) {
    case dom::element_type::ARRAY: {
      Napi::Array arr = Napi::Array::New(env);
      std::size_t i = 0;
      for (dom::element child : dom::array(element)) {
        arr.Set(i, makeJSONObject(env, child));
        i++;
      }
      return arr;
    }
    case dom::element_type::OBJECT: {
      Napi::Object obj = Napi::Object::New(env); // {
      for (auto field : dom::object(element)) {
        // TODO not 8-bit clean, but likely faster than making a new JS string ... figure out how to
        // do this without allocating a whole new std::string
        obj.Set(field.key.data(), makeJSONObject(env, field.value));
      }
      return obj;
    }
    case dom::element_type::STRING: {
      std::string_view str = element;
      return Napi::String::New(env, str.data(), str.length());
    }
    case dom::element_type::INT64:
      return Napi::Value::From<int64_t>(env, element);
    case dom::element_type::UINT64:
      return Napi::Value::From<uint64_t>(env, element);
    case dom::element_type::DOUBLE:
      return Napi::Value::From<double>(env, element);
    case dom::element_type::BOOL:
      return Napi::Value::From<bool>(env, element);
    case dom::element_type::NULL_VALUE:
      return env.Null();
  }
  Napi::Error::New(env, "Internal error: Unexpected JSON type from simdjson").ThrowAsJavaScriptException();
  return env.Null();
}

static std::vector<std::string> parseKeyPath(std::string str) {
    char * cstr = const_cast<char *>(str.c_str());
    char * current;
    std::string delimiters = ".[]";
    std::vector<std::string> arr;
    current = strtok(cstr, delimiters.c_str());
    while(current != NULL) {
        arr.push_back(current);
        current=strtok(NULL, delimiters.c_str());
    }
    return arr;
}

static bool isNumber(std::string s) {
  for(std::string::size_type i = 0; i < s.size(); ++i) {
    if (!isdigit(s[i])) return false;
  }
  return true;
}

Napi::Value simdjsonnode::findKeyPath(Napi::Env env, std::vector<std::string> subpaths, dom::element element) {
  if (subpaths.empty()) return makeJSONObject(env, element).As<Napi::Object>();
  std::string subpath = subpaths.front();
  subpaths.erase(subpaths.begin());
  switch (element.type()) {
    case dom::element_type::ARRAY: {
      if (!isNumber(subpath)) {
        std::string error = "Invalid keypath " + subpath + ": must be a number when accessing an array";
        Napi::Error::New(env, error).ThrowAsJavaScriptException();
      }
      return findKeyPath(env, subpaths, element.at(std::stoi(subpath)));
    }
    case dom::element_type::OBJECT: {
      return findKeyPath(env, subpaths, element.at_key(subpath));
    }
    default: {
      std::string error = "Invalid keypath " + subpath + ": keys only work on arrays and objects";
      Napi::Error::New(env, error).ThrowAsJavaScriptException();
      return env.Null();
    }
  }
}

Napi::Value simdjsonnode::ValueForKeyPathWrapped(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string path = info[0].As<Napi::String>();
  Napi::Object _this = info.This().As<Napi::Object>();
  Napi::External<dom::document> buffer = _this.Get("buffer").As<Napi::External<dom::document>>();
  dom::document * doc = buffer.Data();
  try {
    return simdjsonnode::findKeyPath(env, parseKeyPath(path), doc->root());
  } catch (simdjson_error &error) {
    Napi::Error::New(env, error_message(error.error())).ThrowAsJavaScriptException();
    return env.Null();
  }
}

Napi::Value simdjsonnode::ParseWrapped(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  padded_string input = getInputAsPaddedString(info[0]);
  try {

    dom::parser parser;
    return makeJSONObject(env, parser.parse(input));

  } catch (simdjson_error &error) {
    Napi::Error::New(env, error_message(error.error())).ThrowAsJavaScriptException();
    return env.Null();
  }
}

Napi::Object simdjsonnode::LazyParseWrapped(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  padded_string input = getInputAsPaddedString(info[0]);
  dom::parser parser;
  error_code error = parser.parse(input).error();
  if (error) {
    Napi::Error::New(env, error_message(error)).ThrowAsJavaScriptException();
    return Napi::Object::New(env);
  }
  Napi::External<dom::document> buffer = Napi::External<dom::document>::New(env, new dom::document(std::move(parser.doc)),
    [](Napi::Env /*env*/, dom::document * doc) {
      delete doc;
    });
  Napi::Object result = Napi::Object::New(env);
  result.Set("buffer", buffer);
  result.Set("valueForKeyPath", Napi::Function::New(env, simdjsonnode::ValueForKeyPathWrapped));
  return result;  
}

Napi::Value simdjsonnode::FindChunkBoundariesWrapped(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsBuffer() || !info[1].IsString() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "findChunkBoundaries(buffer, dotPath, maxChunkBytes)").ThrowAsJavaScriptException();
    return env.Null();
  }

  auto buf = info[0].As<Napi::Buffer<uint8_t>>();
  std::string pathStr = info[1].As<Napi::String>().Utf8Value();
  size_t maxChunkBytes = (size_t)info[2].As<Napi::Number>().Int64Value();

  const uint8_t* d = buf.Data();
  size_t len = buf.ByteLength();

  // Split dot-separated path
  std::vector<std::string> segments;
  std::string seg;
  for (char c : pathStr) {
    if (c == '.') { if (!seg.empty()) { segments.push_back(seg); seg.clear(); } }
    else { seg += c; }
  }
  if (!seg.empty()) segments.push_back(seg);

  if (segments.empty()) {
    Napi::TypeError::New(env, "findChunkBoundaries: empty path").ThrowAsJavaScriptException();
    return env.Null();
  }

  // Navigate to the target value
  size_t arrayPos = navigatePath(d, len, segments);
  if (arrayPos == SIZE_MAX) {
    Napi::Error::New(env, "findChunkBoundaries: path not found").ThrowAsJavaScriptException();
    return env.Null();
  }

  arrayPos = skipWhitespace(d, len, arrayPos);
  if (arrayPos >= len || d[arrayPos] != '[') {
    Napi::Error::New(env, "findChunkBoundaries: value at path is not an array").ThrowAsJavaScriptException();
    return env.Null();
  }
  arrayPos++; // skip '['

  // Scan and collect chunks
  auto chunks = scanArrayChunks(d, len, arrayPos, maxChunkBytes);

  Napi::Array result = Napi::Array::New(env, chunks.size());
  for (size_t i = 0; i < chunks.size(); i++) {
    Napi::Array pair = Napi::Array::New(env, 2);
    pair.Set(0u, Napi::Number::New(env, (double)chunks[i].first));
    pair.Set(1u, Napi::Number::New(env, (double)chunks[i].second));
    result.Set((uint32_t)i, pair);
  }
  return result;
}

Napi::Object simdjsonnode::Init(Napi::Env env, Napi::Object exports) {
  exports.Set("isValid", Napi::Function::New(env, simdjsonnode::IsValidWrapped));
  exports.Set("parse", Napi::Function::New(env, simdjsonnode::ParseWrapped));
  exports.Set("lazyParse", Napi::Function::New(env, simdjsonnode::LazyParseWrapped));
  exports.Set("findChunkBoundaries", Napi::Function::New(env, simdjsonnode::FindChunkBoundariesWrapped));
  return exports;
}
