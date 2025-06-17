# Crypto HFT Bot (C++) - MVP Guide

This project is a high-frequency crypto trading bot built in C++ that connects to Binance, maintains a live order book, and places limit orders using a basic market-making strategy. It prioritizes low latency, clean architecture, and real PnL tracking.

## MVP Objective

Build a low-latency crypto trading bot in C++ that connects to Binance via WebSocket, maintains a real-time order book, runs a simple market-making strategy, and places/cancels limit orders via REST. The bot should be self-contained, log all trades and positions, and be deployable to a production Linux VPS.

## Strategy Used: Market Making

### What is Market Making?

Market making involves continuously placing both buy and sell limit orders around the current market price. You profit from the spread when both your buy and sell orders get filled.

* If the market moves up: Your buy order fills first, then the market hits your sell order.
* If the market moves down: Your sell order fills first, then your buy order executes at a lower price.

### Example:

* Best Bid = 29,950
* Best Ask = 30,050
* Your Limit Buy = 29,955
* Your Limit Sell = 30,045

If both orders are filled, you capture a \$90 spread per BTC (minus fees).

### How Limit Orders Work

A limit order is an instruction to buy or sell at a specific price or better:

* **Limit Buy**: Executes only if the price drops to or below your limit.
* **Limit Sell**: Executes only if the price rises to or above your limit.

This bot only places passive limit orders. It never crosses the spread with a market order.

---

## Quick Setup (Local Development)

**Before following the full installation guide below, you can get started locally:**

1. **Copy the configuration template:**
   ```bash
   cp config.template config.txt
   ```

2. **Edit `config.txt` and add your Binance API credentials:**
   - Get API keys from [Binance API Management](https://www.binance.com/en/my/settings/api-management)
   - Replace `your_api_key_here` and `your_secret_key_here` with your actual keys
   - Keep `USE_TESTNET=true` for safe testing

3. **The bot is now configured for ETH/USDT trading with:**
   - Order size: 0.01 ETH
   - Max inventory: 0.1 ETH
   - Spread threshold: 5 basis points

**Note:** The `config.txt` file (which you create from the template) contains your actual API keys and should never be committed to git. It's already in `.gitignore`.

---

## Installation & Setup Guide

step 1: Create a new Ubuntu 22.04 VPS with at least 2 vCPUs and low ping to Binance (choose Tokyo if trading on Binance Global). Use DigitalOcean or Vultr High Frequency.

step 2: SSH into your VPS.

step 3: Run `sudo apt update && sudo apt upgrade -y`

step 4: Install build tools:

```bash
sudo apt install -y build-essential cmake git curl unzip pkg-config
```

step 5: Install networking + SSL libraries:

```bash
sudo apt install -y libcurl4-openssl-dev libssl-dev libwebsockets-dev
```

step 6: Install Redis and its C++ client dependency:

```bash
sudo apt install -y redis-server libhiredis-dev
```

step 7: Clone and install uWebSockets:

```bash
git clone https://github.com/uNetworking/uWebSockets.git
cd uWebsockets
make
sudo make install
cd ..
```

step 8: Install the nlohmann JSON library:

```bash
sudo apt install -y nlohmann-json3-dev
```

step 9: Create a directory `mkdir -p ~/hft-bot/{src,include,logs,data}` and `cd ~/hft-bot`

step 10: Open a Binance account, go to [https://www.binance.com/en/my/settings/api-management](https://www.binance.com/en/my/settings/api-management), and create an API key and secret. Copy `config.template` to `config.txt` and fill in your API credentials.

step 11: Create a WebSocket client in C++ using uWebSockets that connects to:

```
wss://stream.binance.com:9443/ws/btcusdt@depth10@100ms
```

step 12: Parse the incoming JSON using `nlohmann::json` and update an in-memory order book using two `std::map<double, double>` structures for bid and ask.

step 13: Write a function that logs every order book update to a file in `~/hft-bot/logs/orderbook.log` with a timestamp.

step 14: Write strategy logic that compares best bid and ask and computes if the spread is greater than a set threshold (e.g. 5bps).

step 15: If the spread is acceptable, generate a limit buy order below best bid and a limit sell order above best ask.

step 16: Use `libcurl` to send a POST request to the Binance REST API endpoint:

```
https://api.binance.com/api/v3/order
```

step 17: Include the required headers:

* `X-MBX-APIKEY: <your_api_key>`

step 18: Include parameters:

* `symbol`, `side`, `type`, `timeInForce`, `quantity`, `price`, `timestamp`

step 19: Create a query string, then sign it using HMAC-SHA256 with your Binance API secret using OpenSSL (`<openssl/hmac.h>`).

step 20: Append the signature to the request and send the full URL with parameters and headers using cURL.

step 21: Parse the response JSON and store the `orderId` and status in a Redis hash under the key `open_orders`.

step 22: Implement a loop to check for order status every 5 seconds using:

```
GET https://api.binance.com/api/v3/order
```

step 23: When an order is filled, update your inventory in Redis and append the trade to `~/hft-bot/logs/fills.log`.

step 24: Write a PnL tracker that calculates unrealized PnL based on current best bid/ask and your position, and logs to `~/hft-bot/logs/pnl.log`.

step 25: Set a limit for max inventory (e.g., 0.01 BTC), and stop placing new orders if your position exceeds that.

step 26: Add a circuit breaker: if daily drawdown exceeds a threshold (e.g., −\$20), cancel all orders and stop trading.

step 27: Write a health monitor that pings Binance’s `/api/v3/ping` every 30 seconds and logs results.

step 28: Test the full pipeline on Binance’s testnet by modifying URLs to use:

```
https://testnet.binance.vision
```

step 29: Once working, change `.env` to use real API keys and mainnet URLs.

step 30: Install `tmux` and run your bot in a persistent terminal session:

```bash
sudo apt install -y tmux
tmux new -s hft
```

step 31: Start Redis with `sudo systemctl start redis`

step 32: Compile your project with `g++` or CMake (C++17 enabled), link against `libcurl`, `libssl`, `libcrypto`, `libhiredis`, and `uWS`.

step 33: Launch your bot binary inside `tmux` and tail logs with:

```bash
tail -f logs/pnl.log logs/orderbook.log logs/fills.log
```

step 34: After 24–48 hours of live trading, analyze logs for:

* win rate
* average spread
* fill ratios
* latency from book update to order sent

step 35: Tune strategy: adjust order offsets, frequency, order size, and cancel logic based on live performance.

step 36: Deploy your code via `git` to a private repo, push changes from local to VPS for updates.

step 37: Set up a daily cron job to rotate and compress logs in `logs/`.

step 38: Add automatic restart on crash using a watchdog script or systemd service.

step 39: Track your capital performance over time using a CSV or SQLite file that logs daily PnL snapshots.

step 40: As capital grows, expand to other pairs (e.g., ETH/USDT, SOL/USDT) and add multi-symbol support to your engine.


Absolutely. Here's a detailed **MVP (Minimum Viable Product) description** for your C++ crypto HFT bot — perfect for pasting into Cursor or using to plan your repo structure and implementation flow:

---

## **Project: High-Frequency Crypto Trading Bot (C++)**

### **MVP Objective**

Build a low-latency crypto trading bot in C++ that connects to Binance via WebSocket, maintains a real-time order book, runs a simple market-making strategy, and places/cancels limit orders via REST. The bot should be self-contained, log all trades and positions, and be deployable to a production Linux VPS.

---

### **Core Features**

1. **Real-Time Order Book Listener**

   * Connect to Binance WebSocket `wss://stream.binance.com:9443/ws/btcusdt@depth10@100ms`
   * Parse L2 depth data using `nlohmann::json`
   * Maintain sorted bid/ask order book in memory

2. **Market-Making Strategy Engine**

   * Monitor current best bid and ask
   * Place limit buy and sell orders within configurable spread
   * Update quotes periodically (e.g., every 200ms)
   * Cancel stale or unfilled orders

3. **Order Execution Engine**

   * Use Binance REST API to place/cancel orders
   * Sign all requests using HMAC-SHA256 (via OpenSSL)
   * Track order states (NEW, FILLED, PARTIALLY\_FILLED, CANCELED)
   * Store open orders in Redis for fast access

4. **Position & Risk Management**

   * Track net position (in BTC/USDT)
   * Enforce inventory limits (e.g., ±0.01 BTC)
   * Stop trading if drawdown exceeds threshold (e.g., −\$20)

5. **Logging & Monitoring**

   * Log order book snapshots, trades, and PnL to `logs/`
   * Write health logs for WebSocket/REST latency and failures
   * Export daily PnL snapshots to CSV

6. **Deployment Support**

   * Run inside `tmux` or via `systemd`
   * Easily configurable with `.env` file
   * Fully compatible with Binance testnet or mainnet

---

### **Tech Stack**

* **Language**: C++17
* **Real-Time Networking**: uWebSockets
* **JSON Parsing**: nlohmann/json
* **REST Calls**: libcurl
* **Signature Generation**: OpenSSL (HMAC-SHA256)
* **Data Storage**: Redis (via hiredis)
* **Logging**: std::ofstream
* **Build Tool**: CMake

---

### **Folder Structure**

```
hft-bot/
├── src/                # core source code (main, sockets, strategy)
├── include/            # headers for components
├── logs/               # pnl.log, fills.log, orderbook.log
├── data/               # config files, historical data (optional)
├── .env                # API keys and config
├── CMakeLists.txt
└── README.md
```

---

### **Stretch Goals (Post-MVP)**

* Support multiple trading pairs
* Add second exchange for cross-exchange arbitrage
* Implement latency tracking between market data and order placement
* Build backtester using historical WebSocket dumps

---

Let me know if you want this added to your repo as a `README.md` or `MVP_PLAN.md` file.
