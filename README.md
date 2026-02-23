# nanocode-cpp

A C++23 rewrite of nanocode, a minimal agentic coding assistant. This version natively implements tool usage and API interactions without Python dependencies, using Boost Asio for asynchronous operations.

## Features

- Complete native C++ implementation
- Interactive agentic loop
- Built-in tools: read, write, edit, glob, grep, bash
- API support for Gemini, Anthropic, and OpenRouter
- Asynchronous networking using Boost coroutines

## Prerequisites

- CMake 3.20 or newer
- C++23 compatible compiler (e.g., Apple Clang 15+)
- Boost libraries (Components: json)
- OpenSSL

## Build Instructions

1. Create a build directory:
   ```bash
   mkdir build
   cd build
   ```

2. Run CMake and build:
   ```bash
   cmake ..
   make
   ```

## Usage

Set one of the following environment variables:
- `GEMINI_API_KEY`
- `ANTHROPIC_API_KEY`
- `OPENROUTER_API_KEY`

Optionally, set the `MODEL` environment variable to override defaults.

Run the executable:
```bash
./build/nanocode
```

## License

MIT
