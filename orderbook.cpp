#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cmath>
#include <deque>
#include <thread>
#include <unordered_map>
#include <chrono>
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
const double BASE_PRICE     = 44000.0;
const double TICK_SIZE      = 0.01;
const int    LADDER_SIZE    = 5000000;
const double WALL_THRESHOLD = 5.0;
const double WALL_RANGE     = 2000.0;

vector<double> bidLadder(LADDER_SIZE, 0.0);
vector<double> askLadder(LADDER_SIZE, 0.0);

int bestBidIdx = 0;
int bestAskIdx = LADDER_SIZE - 1;

unordered_map<int, double> bidWalls;
unordered_map<int, double> askWalls;

vector<string> recentWallEvents;

// ===== TOXICITY TRACKING =====
const int TOXICITY_WINDOW = 100;
deque<double> aggressiveBuyVol;
deque<double> aggressiveSellVol;
double totalAggressiveBuy  = 0.0;
double totalAggressiveSell = 0.0;
double updateAggressiveBuy  = 0.0;
double updateAggressiveSell = 0.0;

// ===== CSV LOGGING =====
ofstream csvFile;
long long updateCount = 0;

void initCSV() {
    csvFile.open("pulse_data.csv");
    csvFile << "timestamp_ms,"
            << "update_count,"
            << "mid_price,"
            << "best_bid,"
            << "best_ask,"
            << "spread,"
            << "latency_ns,"
            << "num_updates,"
            << "imbalance,"
            << "buy_aggression,"
            << "sell_aggression,"
            << "aggression_ratio,"
            << "nearest_ask_wall_price,"
            << "nearest_ask_wall_qty,"
            << "nearest_bid_wall_price,"
            << "nearest_bid_wall_qty,"
            << "wall_event"
            << "\n";
    csvFile.flush();
}

inline int priceToIndex(double price) {
    return (int)round((price - BASE_PRICE) / TICK_SIZE);
}

inline double indexToPrice(int idx) {
    return BASE_PRICE + idx * TICK_SIZE;
}

void updateLevel(bool isBid, double price, double qty) {
    int idx = priceToIndex(price);
    if (idx < 0 || idx >= LADDER_SIZE) return;

    auto& ladder = isBid ? bidLadder : askLadder;
    auto& walls  = isBid ? bidWalls  : askWalls;
    string side  = isBid ? "BID" : "ASK";

    double prevQty = ladder[idx];
    ladder[idx] = qty;

    if (isBid) {
        if (qty > 0.0 && idx > bestBidIdx) bestBidIdx = idx;
    } else {
        if (qty > 0.0 && idx < bestAskIdx) bestAskIdx = idx;
    }

    if (!isBid && qty < prevQty && prevQty > 0.0)
        updateAggressiveBuy  += prevQty - qty;
    if (isBid  && qty < prevQty && prevQty > 0.0)
        updateAggressiveSell += prevQty - qty;

    if (qty >= WALL_THRESHOLD && prevQty < WALL_THRESHOLD) {
        walls[idx] = qty;
        string event = "[WALL APPEARED] " + side + " $" +
            to_string((int)round(indexToPrice(idx) * 100) / 100.0).substr(0, 9) +
            "  " + to_string(qty).substr(0, 4) + " BTC";
        recentWallEvents.push_back(event);
        if (recentWallEvents.size() > 3) recentWallEvents.erase(recentWallEvents.begin());
    }

    if (prevQty >= WALL_THRESHOLD && qty < WALL_THRESHOLD) {
        walls.erase(idx);
        string event = "[WALL GONE !!!] " + side + " $" +
            to_string((int)round(indexToPrice(idx) * 100) / 100.0).substr(0, 9) +
            "  was " + to_string(prevQty).substr(0, 4) + " BTC << SPOOF?";
        recentWallEvents.push_back(event);
        if (recentWallEvents.size() > 3) recentWallEvents.erase(recentWallEvents.begin());
    }
}

void updateToxicityWindow() {
    aggressiveBuyVol.push_back(updateAggressiveBuy);
    aggressiveSellVol.push_back(updateAggressiveSell);
    totalAggressiveBuy  += updateAggressiveBuy;
    totalAggressiveSell += updateAggressiveSell;

    if ((int)aggressiveBuyVol.size() > TOXICITY_WINDOW) {
        totalAggressiveBuy  -= aggressiveBuyVol.front();
        totalAggressiveSell -= aggressiveSellVol.front();
        aggressiveBuyVol.pop_front();
        aggressiveSellVol.pop_front();
    }

    updateAggressiveBuy  = 0.0;
    updateAggressiveSell = 0.0;
}

uint64_t rdtsc() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

double ticksToNanos(uint64_t ticks, uint64_t freq) {
    return (double)ticks * 1e9 / (double)freq;
}

void logToCSV(double midPrice, double bestBid, double bestAsk,
              double spread, double latencyNs, int numUpdates,
              double imbalance, double buyAggression, double sellAggression,
              double aggressionRatio,
              double nearestAskWallPrice, double nearestAskWallQty,
              double nearestBidWallPrice, double nearestBidWallQty) {

    // Timestamp in milliseconds since epoch
    auto now = chrono::system_clock::now();
    long long ms = chrono::duration_cast<chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Latest wall event if any
    string wallEvent = recentWallEvents.empty() ? "" : recentWallEvents.back();
    // Escape commas in wall event
    for (auto& c : wallEvent) if (c == ',') c = ';';

    csvFile << ms << ","
            << updateCount << ","
            << fixed << setprecision(2) << midPrice << ","
            << fixed << setprecision(2) << bestBid  << ","
            << fixed << setprecision(2) << bestAsk  << ","
            << fixed << setprecision(2) << spread   << ","
            << fixed << setprecision(0) << latencyNs << ","
            << numUpdates << ","
            << fixed << setprecision(4) << imbalance << ","
            << fixed << setprecision(4) << buyAggression  << ","
            << fixed << setprecision(4) << sellAggression << ","
            << fixed << setprecision(4) << aggressionRatio << ","
            << fixed << setprecision(2) << nearestAskWallPrice << ","
            << fixed << setprecision(2) << nearestAskWallQty   << ","
            << fixed << setprecision(2) << nearestBidWallPrice << ","
            << fixed << setprecision(2) << nearestBidWallQty   << ","
            << wallEvent
            << "\n";
    csvFile.flush();
}

void printOrderBook(double latencyNs, int numUpdates) {
    system("clear");
    updateCount++;

    vector<pair<double,double>> topAsks;
    for (int i = bestAskIdx; i < LADDER_SIZE && topAsks.size() < 5; ++i) {
        if (askLadder[i] > 0.0) topAsks.push_back({indexToPrice(i), askLadder[i]});
    }

    vector<pair<double,double>> topBids;
    for (int i = bestBidIdx; i >= 0 && topBids.size() < 5; --i) {
        if (bidLadder[i] > 0.0) topBids.push_back({indexToPrice(i), bidLadder[i]});
    }

    double midPrice = (!topBids.empty() && !topAsks.empty())
                      ? (topBids.front().first + topAsks.front().first) / 2.0
                      : 69000.0;
    double bestBid  = !topBids.empty() ? topBids.front().first : 0.0;
    double bestAsk  = !topAsks.empty() ? topAsks.front().first : 0.0;
    double spread   = bestAsk - bestBid;

    double bidVolume = 0.0, askVolume = 0.0;
    for (auto& b : topBids) bidVolume += b.second;
    for (auto& a : topAsks) askVolume += a.second;
    double imbalance = (bidVolume + askVolume > 0)
                       ? bidVolume / (bidVolume + askVolume) : 0.5;

    string imbalanceStr;
    if      (imbalance > 0.6) imbalanceStr = "BUY PRESSURE  >>>>";
    else if (imbalance < 0.4) imbalanceStr = "<<<< SELL PRESSURE";
    else                       imbalanceStr = "      NEUTRAL      ";

    double totalAggressive = totalAggressiveBuy + totalAggressiveSell;
    double buyAggression   = totalAggressiveBuy;
    double sellAggression  = totalAggressiveSell;
    double aggressionRatio = (totalAggressive > 0)
                             ? buyAggression / totalAggressive : 0.5;

    string toxicityStr;
    if      (aggressionRatio > 0.65) toxicityStr = "AGGRESSIVE BUYERS";
    else if (aggressionRatio < 0.35) toxicityStr = "AGGRESSIVE SELLERS";
    else                              toxicityStr = "MIXED AGGRESSION";

    pair<double,double> nearestBidWall = {0, 0};
    pair<double,double> nearestAskWall = {0, 0};

    for (auto& w : bidWalls) {
        double wp = indexToPrice(w.first);
        if (abs(wp - midPrice) < WALL_RANGE) {
            if (nearestBidWall.first == 0 || abs(wp - midPrice) < abs(nearestBidWall.first - midPrice))
                nearestBidWall = {wp, w.second};
        }
    }
    for (auto& w : askWalls) {
        double wp = indexToPrice(w.first);
        if (abs(wp - midPrice) < WALL_RANGE) {
            if (nearestAskWall.first == 0 || abs(wp - midPrice) < abs(nearestAskWall.first - midPrice))
                nearestAskWall = {wp, w.second};
        }
    }

    // Log to CSV
    logToCSV(midPrice, bestBid, bestAsk, spread, latencyNs, numUpdates,
             imbalance, buyAggression, sellAggression, aggressionRatio,
             nearestAskWall.first, nearestAskWall.second,
             nearestBidWall.first, nearestBidWall.second);

    // ===== DISPLAY =====
    cout << "============= PULSE =============" << endl;
    cout << "Price:    $" << fixed << setprecision(2) << midPrice << endl;
    cout << "Spread:   $" << fixed << setprecision(2) << spread   << endl;
    cout << "Latency:   " << fixed << setprecision(0) << latencyNs << " ns" << endl;
    cout << "Updates:   " << numUpdates << endl;
    cout << "Logged:    " << updateCount << " rows -> pulse_data.csv" << endl;

    cout << "\n";
    for (auto it = topAsks.rbegin(); it != topAsks.rend(); ++it) {
        cout << "  $" << fixed << setprecision(2) << it->first
             << "  |  " << fixed << setprecision(4) << it->second
             << " BTC  <-- SELL" << endl;
    }
    cout << "  --------- SPREAD: $" << fixed << setprecision(2) << spread << " ---------" << endl;
    for (auto& b : topBids) {
        cout << "  $" << fixed << setprecision(2) << b.first
             << "  |  " << fixed << setprecision(4) << b.second
             << " BTC  <-- BUY" << endl;
    }

    cout << "\nImbalance: " << fixed << setprecision(4) << imbalance
         << "  " << imbalanceStr << endl;

    cout << "\nFlow Toxicity (last " << TOXICITY_WINDOW << " updates):" << endl;
    cout << "  Buy aggression:  " << fixed << setprecision(4) << buyAggression  << " BTC" << endl;
    cout << "  Sell aggression: " << fixed << setprecision(4) << sellAggression << " BTC" << endl;
    cout << "  Ratio: " << fixed << setprecision(4) << aggressionRatio
         << "  " << toxicityStr << endl;

    cout << "\n----- NEAREST WALLS -----" << endl;
    if (nearestAskWall.first > 0) {
        int ticks = (int)round((nearestAskWall.first - midPrice) / TICK_SIZE);
        cout << "  ASK WALL  $" << fixed << setprecision(2) << nearestAskWall.first
             << "  |  " << fixed << setprecision(2) << nearestAskWall.second
             << " BTC  (" << ticks << " ticks away)" << endl;
    } else {
        cout << "  ASK WALL  none nearby" << endl;
    }
    if (nearestBidWall.first > 0) {
        int ticks = (int)round((midPrice - nearestBidWall.first) / TICK_SIZE);
        cout << "  BID WALL  $" << fixed << setprecision(2) << nearestBidWall.first
             << "  |  " << fixed << setprecision(2) << nearestBidWall.second
             << " BTC  (" << ticks << " ticks away)" << endl;
    } else {
        cout << "  BID WALL  none nearby" << endl;
    }

    cout << "\n----- LAST ALERTS -----" << endl;
    if (recentWallEvents.empty()) {
        cout << "  none yet" << endl;
    } else {
        for (auto& e : recentWallEvents) cout << "  " << e << endl;
    }

    cout << "=================================" << endl;
}

int main() {
    initCSV();

    uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    while (true) {  // outer reconnection loop
        try {
            net::io_context ioc;
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();

            tcp::resolver resolver(ioc);
            websocket::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ctx);

            string host   = "stream.binance.com";
            string port   = "9443";
            string target = "/ws/btcusdt@depth";

            auto const results = resolver.resolve(host, port);
            net::connect(get_lowest_layer(ws), results.begin(), results.end());

            if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str()))
                throw beast::system_error(beast::error_code(
                    static_cast<int>(::ERR_get_error()),
                    net::error::get_ssl_category()));

            ws.next_layer().handshake(ssl::stream_base::client);
            ws.handshake(host, target);

            cout << "Connected to Binance..." << endl;

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

                updateToxicityWindow();
                printOrderBook(latencyNs, numUpdates);
            }
        }
        catch (exception const& e) {
            cerr << "Disconnected: " << e.what() << endl;
            cerr << "Reconnecting in 2 seconds..." << endl;
            this_thread::sleep_for(chrono::seconds(2));
        }
    }

    csvFile.close();
    return 0;
}