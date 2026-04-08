# Crypto HFT Bot

A low-latency cryptocurrency market-making bot written in C++17, targeting the Coinbase Advanced Trade API. Streams real-time L2 order book data over WebSocket, runs a configurable market-making strategy with inventory skew, and enforces position/PnL risk limits with a circuit breaker.

## Architecture

```
WebSocket (L2) ──► MarketDataFeed ──► SPSCQueue ──► MarketMakingStrategy
                                                          │
                                                    OrderExecutor
                                                          │
                                                    OrderManager ◄── PnL tracking
                                                          │
                                              ┌───────────┴───────────┐
                                         RiskManager          MetricsCollector
```

The engine orchestrates four worker threads:

| Thread | Role |
|---|---|
| **Market data** | Parses L2 snapshots/updates, maintains a sorted book, publishes BBO to a lock-free queue |
| **Order engine** | Consumes market data, generates signals via the strategy, places order ladders, processes fills |
| **Risk** | Monitors position limits, daily loss, drawdown; triggers circuit breaker on breach |
| **Metrics** | Prints 5s/10s trading summaries, tracks order latency and throughput |

## Requirements

- C++17 compiler (GCC 9+ / Clang 10+ / Apple Clang 14+)
- CMake 3.12+
- OpenSSL
- libwebsockets
- nlohmann/json
- [jwt-cpp](https://github.com/Thalhammer/jwt-cpp) (included as submodule in `jwt-cpp/`)

### Install dependencies

```bash
# macOS
brew install cmake openssl nlohmann-json libwebsockets

# Ubuntu/Debian
sudo apt install build-essential cmake libssl-dev \
  nlohmann-json3-dev libwebsockets-dev
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

This produces `build/crypto_hft_engine`.

## Setup

1. Copy the example config and fill in your Coinbase Advanced Trade API credentials (ECDSA key pair):

```bash
cp config.example config.txt
```

2. Edit `config.txt` with your `COINBASE_API_KEY` and `COINBASE_SECRET_KEY`.

3. Run:

```bash
./build/crypto_hft_engine
```

Press `Ctrl+C` for graceful shutdown. A session summary is written to `logs/session_summary.log`.

## Configuration

All parameters live in `config.txt`. See `config.example` for the full template.

### API

| Parameter | Description |
|---|---|
| `TRADING_SYMBOL` | Trading pair (default: `ETH-USD`) |
| `COINBASE_API_KEY` | Coinbase Advanced Trade API key |
| `COINBASE_SECRET_KEY` | ECDSA private key (PEM, `\n`-escaped) |
| `COINBASE_WS_URL` | WebSocket endpoint |

### Order Sizing

| Parameter | Default | Description |
|---|---|---|
| `ORDER_SIZE` | 0.001 | Base order quantity (ETH) |
| `MAX_INVENTORY` | 0.015 | Max position before the engine stops adding |
| `ORDER_RATE_LIMIT` | 300 | Max orders per second |

### Strategy

| Parameter | Default | Description |
|---|---|---|
| `TICK_SIZE` | 0.01 | Minimum price increment |
| `SPREAD_OFFSET_TICKS` | 0.25 | Ticks away from BBO to place orders |
| `MIN_SPREAD_TICKS` | 0.5 | Minimum spread enforced between bid and ask |
| `MAX_NEUTRAL_POSITION` | 0.01 | Position threshold before inventory skew kicks in |
| `INVENTORY_CEILING` | 0.02 | Position at which order sizes are fully penalized |
| `ORDER_LADDER_LEVELS` | 5 | Number of price levels per side |
| `ORDER_ENGINE_HZ` | 2000 | Order engine tick rate (Hz) |

### Risk

| Parameter | Default | Description |
|---|---|---|
| `POSITION_LIMIT_ETHUSDT` | 0.02 | Hard position limit per symbol |
| `MAX_DAILY_LOSS_LIMIT` | 3.0 | Daily loss circuit breaker (USD) |
| `MAX_DRAWDOWN_LIMIT` | 2.0 | Peak-to-trough drawdown limit (USD) |

## Project Structure

```
include/
  core/           types.h, config.h, logger.h, spsc_queue.h
  data/           market_data.h, websocket_client.h
  strategy/       market_maker.h (HFTSignal, MarketMakingStrategy)
  execution/      executor.h (HFTOrder, OrderExecutor)
  order/          order_manager.h (OrderManager, OrderResponse)
  risk/           risk_manager.h (RiskManager, RiskStatus, RiskEvent)
  metrics/        metrics.h (AtomicHFTMetrics, MetricsCollector)
  engine.h        thin orchestrator

src/
  main.cpp        entry point + signal handling
  engine.cpp      thread lifecycle, component wiring
  core/           config.cpp, logger.cpp
  data/           market_data_feed.cpp, websocket_client.cpp
  strategy/       market_maker.cpp
  execution/      executor.cpp
  order/          order_manager.cpp
  risk/           risk_manager.cpp
  metrics/        metrics.cpp

tests/
  smoke_test.cpp  end-to-end pipeline verification
```

25 source files, ~2500 lines total. Longest file: 390 lines (websocket_client.cpp). Most files: 50-150 lines.

## Testing

```bash
# Build and run the smoke test (no API credentials needed)
make smoke_test
cd .. && ./build/smoke_test
```

The smoke test exercises the full pipeline -- strategy signal generation, order ladder placement, fill simulation, PnL calculation (long/short/zero-crossing), inventory skew, risk limits, SPSC queue overflow, and latency metrics -- without requiring a WebSocket connection.
