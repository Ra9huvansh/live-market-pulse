# Live-Market-Pulse

A low-latency C++ order book engine that ingests live market data from Binance's WebSocket feed, reconstructs full bid/ask depth in memory, and measures update latency at nanosecond precision.

## What it does

- Connects to Binance's live WebSocket stream for BTC/USDT
- Reconstructs the full order book in memory from real-time diff updates
- Displays top 5 bids and asks with live spread calculation
- Measures order book update latency in nanoseconds using the CPU's hardware timer (CNTVCT_EL0)
- Tracks batch size per message to analyze exchange feed behavior

## Sample Output
```
===== LIVE BTC/USDT ORDER BOOK =====
Update latency:  322402.00 ns
Levels updated:  86

  $63968.39  |  0.000260 BTC  <-- SELL
  $63968.35  |  0.050170 BTC  <-- SELL
  $63968.34  |  0.000260 BTC  <-- SELL
  $63968.33  |  0.000250 BTC  <-- SELL
  $63968.32  |  2.458840 BTC  <-- SELL
  -------- SPREAD: $0.01 --------
  $63968.31  |  0.546900 BTC  <-- BUY
  $63968.30  |  0.001380 BTC  <-- BUY
  $63968.29  |  0.000830 BTC  <-- BUY
  $63968.00  |  0.000880 BTC  <-- BUY
  $63967.32  |  0.000160 BTC  <-- BUY
====================================
```

## Tech Stack

- C++17
- Boost.Beast — WebSocket client
- Boost.Asio — async networking
- nlohmann/json — JSON parsing
- OpenSSL — TLS encryption

## Build

### Prerequisites
```bash
brew install cmake boost libwebsockets nlohmann-json
```

### Compile
```bash
g++ -o orderbook orderbook.cpp \
  -I/opt/homebrew/include \
  -L/opt/homebrew/lib \
  -lssl -lcrypto \
  -std=c++17
```

### Run
```bash
./orderbook
```

## Roadmap

- [ ] Switch from `std::map` to price ladder array for O(1) updates
- [ ] Order book imbalance detection
- [ ] Large wall detection and spoofing pattern recognition
- [ ] Terminal UI with ncurses — color coded, no flicker
- [ ] ESP32 hardware dashboard — OLED display + RGB LEDs reacting to live market signals
- [ ] Market making simulation with PnL tracking
- [ ] Technical writeup on order book mechanics and latency optimization

## Architecture

Binance streams order book diffs every 100ms. Each message contains a batch of price level changes — additions, modifications, and removals. Pulse receives these diffs over a persistent SSL WebSocket connection, applies them to an in-memory `std::map` structure sorted by price, and renders the top of book in real time.

Latency is measured using the ARM hardware counter register `CNTVCT_EL0` — the same approach used in high-frequency trading systems — giving nanosecond resolution without OS syscall overhead.

## Author

Raghuvansh Rastogi
