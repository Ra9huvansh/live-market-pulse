#include <iostream>
#include <map>
#include <vector>
#include <string>
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

map<double, double, greater<double>> bids;
map<double, double> asks;

uint64_t rdtsc() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

double ticksToNanos(uint64_t ticks, uint64_t freq) {
    return (double)ticks * 1e9 / (double)freq;
}

void updateLevel(bool isBid, double price, double qty) {
    if (isBid) {
        if (qty == 0.0) bids.erase(price);
        else bids[price] = qty;
    } else {
        if (qty == 0.0) asks.erase(price);
        else asks[price] = qty;
    }
}

void printOrderBook(double latencyNs, int numUpdates) {
    system("clear");
    cout << "===== LIVE BTC/USDT ORDER BOOK =====" << endl;
    cout << "Update latency:  " << fixed << setprecision(2) << latencyNs << " ns" << endl;
    cout << "Levels updated:  " << numUpdates << endl;

    // Collect top 5 asks
    vector<pair<double,double>> topAsks;
    for (auto it = asks.begin(); it != asks.end() && topAsks.size() < 5; ++it) {
        topAsks.push_back(*it);
    }

    cout << "\n";
    // Print asks in reverse so lowest ask is closest to spread
    for (auto it = topAsks.rbegin(); it != topAsks.rend(); ++it) {
        cout << "  $" << fixed << setprecision(2) << it->first
             << "  |  " << fixed << setprecision(6) << it->second
             << " BTC  <-- SELL" << endl;
    }

    // Spread
    if (!bids.empty() && !asks.empty()) {
        double spread = asks.begin()->first - bids.begin()->first;
        cout << "  -------- SPREAD: $" << fixed << setprecision(2) << spread << " --------" << endl;
    }

    // Print top 5 bids
    int count = 0;
    for (auto it = bids.begin(); it != bids.end() && count < 5; ++it, ++count) {
        cout << "  $" << fixed << setprecision(2) << it->first
             << "  |  " << fixed << setprecision(6) << it->second
             << " BTC  <-- BUY" << endl;
    }

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