#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

// ===== PRICE LADDER =====
const double BASE_PRICE  = 44000.0;
const double TICK_SIZE   = 0.01;
const int    LADDER_SIZE = 5000000;

vector<double> bidLadder(LADDER_SIZE, 0.0);
vector<double> askLadder(LADDER_SIZE, 0.0);

int bestBidIdx = 0;
int bestAskIdx = LADDER_SIZE - 1;

inline int priceToIndex(double price) {
    return (int)round((price - BASE_PRICE) / TICK_SIZE);
}

inline double indexToPrice(int idx) {
    return BASE_PRICE + idx * TICK_SIZE;
}

void updateLevel(bool isBid, double price, double qty) {
    int idx = priceToIndex(price);
    if (idx < 0 || idx >= LADDER_SIZE) return;

    if (isBid) {
        bidLadder[idx] = qty;
        if (qty > 0.0 && idx > bestBidIdx) bestBidIdx = idx;
    } else {
        askLadder[idx] = qty;
        if (qty > 0.0 && idx < bestAskIdx) bestAskIdx = idx;
    }
}

uint64_t rdtsc() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

double ticksToNanos(uint64_t ticks, uint64_t freq) {
    return (double)ticks * 1e9 / (double)freq;
}

void printOrderBook(double latencyNs, int numUpdates) {
    system("clear");
    cout << "===== LIVE BTC/USDT ORDER BOOK =====" << endl;
    cout << "Update latency:  " << fixed << setprecision(2) << latencyNs << " ns" << endl;
    cout << "Levels updated:  " << numUpdates << endl;

    vector<pair<double,double>> topAsks;
    for (int i = bestAskIdx; i < LADDER_SIZE && topAsks.size() < 5; ++i) {
        if (askLadder[i] > 0.0) topAsks.push_back({indexToPrice(i), askLadder[i]});
    }

    vector<pair<double,double>> topBids;
    for (int i = bestBidIdx; i >= 0 && topBids.size() < 5; --i) {
        if (bidLadder[i] > 0.0) topBids.push_back({indexToPrice(i), bidLadder[i]});
    }

    cout << "\n";
    for (auto it = topAsks.rbegin(); it != topAsks.rend(); ++it) {
        cout << "  $" << fixed << setprecision(2) << it->first
             << "  |  " << fixed << setprecision(6) << it->second
             << " BTC  <-- SELL" << endl;
    }

    if (!topBids.empty() && !topAsks.empty()) {
        double spread = topAsks.front().first - topBids.front().first;
        cout << "  -------- SPREAD: $" << fixed << setprecision(2) << spread << " --------" << endl;
    }

    for (auto& b : topBids) {
        cout << "  $" << fixed << setprecision(2) << b.first
             << "  |  " << fixed << setprecision(6) << b.second
             << " BTC  <-- BUY" << endl;
    }

    // ===== ORDER BOOK IMBALANCE =====
    double bidVolume = 0.0;
    double askVolume = 0.0;

    for (auto& b : topBids) bidVolume += b.second;
    for (auto& a : topAsks) askVolume += a.second;

    double imbalance = bidVolume / (bidVolume + askVolume);

    cout << "\nImbalance: " << fixed << setprecision(4) << imbalance;

    if (imbalance > 0.6)
        cout << "  <<< BUY PRESSURE";
    else if (imbalance < 0.4)
        cout << "  <<< SELL PRESSURE";
    else
        cout << "  <<< NEUTRAL";

    cout << endl;
    cout << "Bid Vol: " << fixed << setprecision(6) << bidVolume
         << " BTC  |  Ask Vol: " << askVolume << " BTC" << endl;

    cout << "\n====================================" << endl;
}

int main() {
    try {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();

        tcp::resolver resolver(ioc);
        websocket::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ctx);

        string host = "stream.binance.com";
        string port = "9443";
        string target = "/ws/btcusdt@depth";

        auto const results = resolver.resolve(host, port);
        net::connect(get_lowest_layer(ws), results.begin(), results.end());

        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str()))
            throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
                net::error::get_ssl_category()));

        ws.next_layer().handshake(ssl::stream_base::client);
        ws.handshake(host, target);

        cout << "Connected to Binance. Receiving live order book..." << endl;

        uint64_t freq;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

        while (true) {
            beast::flat_buffer buffer;
            ws.read(buffer);

            string msg = beast::buffers_to_string(buffer.data());
            auto j = json::parse(msg);

            int numUpdates = j["b"].size() + j["a"].size();

            uint64_t start = rdtsc();

            for (auto& level : j["b"]) {
                double price = stod(level[0].get<string>());
                double qty   = stod(level[1].get<string>());
                updateLevel(true, price, qty);
            }

            for (auto& level : j["a"]) {
                double price = stod(level[0].get<string>());
                double qty   = stod(level[1].get<string>());
                updateLevel(false, price, qty);
            }

            uint64_t end = rdtsc();
            double latencyNs = ticksToNanos(end - start, freq);

            printOrderBook(latencyNs, numUpdates);
        }

        ws.close(websocket::close_code::normal);
    }
    catch (exception const& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}