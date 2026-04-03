# Crypto HFT Bot

A low-latency cryptocurrency market-making bot written in C++17, targeting Coinbase Pro. Supports real-time order book streaming, dynamic spread-based strategies, and configurable risk controls.

## Architecture

```
WebSocket Feed ──► Order Book ──► Strategy Engine ──► Order Manager ──► REST API
                                       │
                                  Risk Manager
```

**Core components:** HFT engine, order manager, risk manager, strategy engine, WebSocket/REST clients, logger.

## Requirements

- C++17 compiler (GCC 9+ / Clang 10+)
- CMake 3.12+
- OpenSSL, libcurl, libwebsockets, nlohmann/json, hiredis
- Redis 6.0+

### Install dependencies

```bash
# macOS
brew install cmake openssl curl nlohmann-json libwebsockets hiredis redis

# Ubuntu/Debian
sudo apt install build-essential cmake libssl-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libwebsockets-dev libhiredis-dev redis-server
```

## Build & Run

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Before running, edit `config.txt` with your Coinbase Pro API credentials, then:

```bash
redis-server --daemonize yes
./crypto_hft_engine              # HFT engine (recommended)
./crypto_hft_bot                 # legacy engine
```

## Configuration

All parameters live in `config.txt`. Key settings:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `INITIAL_CAPITAL` | 50.0 | Starting capital (USD) |
| `ORDER_SIZE` | 0.001 | Order size (ETH) |
| `SPREAD_THRESHOLD_BPS` | 1.0 | Min spread to trade (bps) |
| `ORDER_RATE_LIMIT` | 300 | Max orders/sec |
| `MAX_DAILY_LOSS_LIMIT` | 3.0 | Daily loss limit (USD) |
| `CIRCUIT_BREAKER_LOSS` | 2.5 | Emergency stop level (USD) |
| `PAPER_TRADING` | true | Paper trading mode |
| `USE_TESTNET` | true | Use sandbox API |

## Project Structure

```
├── include/          # Headers (.h)
├── src/              # Implementation (.cpp)
│   ├── hft_main.cpp  # HFT entry point
│   └── main.cpp      # Legacy entry point
├── config.txt        # Runtime configuration
└── CMakeLists.txt    # Build config
```

