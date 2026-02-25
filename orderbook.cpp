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
#ifdef timeout
#undef timeout
#endif
#include <ncurses.h>
#ifdef timeout
#undef timeout
#endif

using namespace std;
using json = nlohmann::json;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

#define COL_HEADER  1
#define COL_ASK     2
#define COL_BID     3
#define COL_SPREAD  4
#define COL_ALERT   5
#define COL_NEUTRAL 6
#define COL_WALL    7

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

const int TOXICITY_WINDOW = 100;
deque<double> aggressiveBuyVol;
deque<double> aggressiveSellVol;
double totalAggressiveBuy  = 0.0;
double totalAggressiveSell = 0.0;
double updateAggressiveBuy  = 0.0;
double updateAggressiveSell = 0.0;

const int IMBALANCE_HISTORY = 60;
deque<double> imbalanceHistory;

const int LATENCY_HISTORY = 60;
deque<double> latencyHistory;

ofstream csvFile;
long long updateCount = 0;

void initCSV() {
    csvFile.open("pulse_data.csv");
    csvFile << "timestamp_ms,update_count,mid_price,best_bid,best_ask,"
            << "spread,latency_ns,num_updates,imbalance,buy_aggression,"
            << "sell_aggression,aggression_ratio,nearest_ask_wall_price,"
            << "nearest_ask_wall_qty,nearest_bid_wall_price,"
            << "nearest_bid_wall_qty,wall_event\n";
    csvFile.flush();
}

void initNcurses() {
    initscr();
    start_color();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    init_pair(COL_HEADER,  COLOR_CYAN,    COLOR_BLACK);
    init_pair(COL_ASK,     COLOR_RED,     COLOR_BLACK);
    init_pair(COL_BID,     COLOR_GREEN,   COLOR_BLACK);
    init_pair(COL_SPREAD,  COLOR_YELLOW,  COLOR_BLACK);
    init_pair(COL_ALERT,   COLOR_MAGENTA, COLOR_BLACK);
    init_pair(COL_NEUTRAL, COLOR_WHITE,   COLOR_BLACK);
    init_pair(COL_WALL,    COLOR_YELLOW,  COLOR_BLACK);
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

    auto now = chrono::system_clock::now();
    long long ms = chrono::duration_cast<chrono::milliseconds>(
        now.time_since_epoch()).count();

    string wallEvent = recentWallEvents.empty() ? "" : recentWallEvents.back();
    for (auto& c : wallEvent) if (c == ',') c = ';';

    csvFile << ms << "," << updateCount << ","
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
            << wallEvent << "\n";
    csvFile.flush();
}

void printAt(int row, int col, int colorPair, const string& s, bool bold = false) {
    if (bold) attron(A_BOLD);
    attron(COLOR_PAIR(colorPair));
    mvprintw(row, col, "%s", s.c_str());
    attroff(COLOR_PAIR(colorPair));
    if (bold) attroff(A_BOLD);
}

void drawImbalanceGraph(int startRow) {
    const int GRAPH_HEIGHT = 8;
    const int GRAPH_WIDTH  = IMBALANCE_HISTORY;

    printAt(startRow, 0, COL_HEADER, "--------- IMBALANCE HISTORY (last 60 updates) ---------");
    startRow++;

    for (int r = 0; r < GRAPH_HEIGHT; r++) {
        double rowVal = 1.0 - (double)r / (GRAPH_HEIGHT - 1);
        char label[8];
        snprintf(label, sizeof(label), "%.1f |", rowVal);
        printAt(startRow + r, 0, COL_NEUTRAL, string(label));

        int col = 6;
        for (int i = 0; i < (int)imbalanceHistory.size(); i++) {
            double val = imbalanceHistory[i];
            int valRow = (int)round((1.0 - val) * (GRAPH_HEIGHT - 1));
            if (valRow == r) {
                int color = (val > 0.6) ? COL_BID : (val < 0.4) ? COL_ASK : COL_SPREAD;
                printAt(startRow + r, col + i, color, "*", true);
            } else {
                printAt(startRow + r, col + i, COL_NEUTRAL, " ");
            }
        }
    }

    string xaxis = "     +";
    for (int i = 0; i < GRAPH_WIDTH; i++) xaxis += "-";
    printAt(startRow + GRAPH_HEIGHT,     0, COL_NEUTRAL, xaxis);
    printAt(startRow + GRAPH_HEIGHT + 1, 0, COL_NEUTRAL,
            "      <-- older                                     newer -->");
}

void drawLatencyGraph(int startRow) {
    const int GRAPH_HEIGHT = 6;
    const int GRAPH_WIDTH  = LATENCY_HISTORY;

    printAt(startRow, 0, COL_HEADER, "--------- LATENCY HISTORY (last 60 updates) -----------");
    startRow++;

    if (latencyHistory.empty()) {
        printAt(startRow, 0, COL_NEUTRAL, "  waiting for data...");
        return;
    }

    double maxLat = *max_element(latencyHistory.begin(), latencyHistory.end());
    maxLat = max(maxLat, 100000.0);
    double scale = ceil(maxLat / 100000.0) * 100000.0;

    for (int r = 0; r < GRAPH_HEIGHT; r++) {
        double rowVal = scale * (1.0 - (double)r / (GRAPH_HEIGHT - 1));
        char label[10];
        if (rowVal >= 1000000.0)
            snprintf(label, sizeof(label), "%.0fm|", rowVal / 1000000.0);
        else
            snprintf(label, sizeof(label), "%.0fk|", rowVal / 1000.0);
        printAt(startRow + r, 0, COL_NEUTRAL, string(label));

        int col = 6;
        for (int i = 0; i < (int)latencyHistory.size(); i++) {
            double val = latencyHistory[i];
            int valRow = (int)round((1.0 - val / scale) * (GRAPH_HEIGHT - 1));
            valRow = max(0, min(GRAPH_HEIGHT - 1, valRow));
            if (valRow == r) {
                int color;
                if      (val < 200000.0) color = COL_BID;
                else if (val < 500000.0) color = COL_SPREAD;
                else                     color = COL_ASK;
                printAt(startRow + r, col + i, color, "*", true);
            } else {
                printAt(startRow + r, col + i, COL_NEUTRAL, " ");
            }
        }
    }

    string xaxis = "     +";
    for (int i = 0; i < GRAPH_WIDTH; i++) xaxis += "-";
    printAt(startRow + GRAPH_HEIGHT,     0, COL_NEUTRAL, xaxis);
    printAt(startRow + GRAPH_HEIGHT + 1, 0, COL_NEUTRAL,
            "      <-- older                                     newer -->");
}

void drawUI(double midPrice, double bestBid, double bestAsk,
            double spread, double latencyNs, int numUpdates,
            double imbalance, double buyAggression, double sellAggression,
            double aggressionRatio,
            vector<pair<double,double>>& topAsks,
            vector<pair<double,double>>& topBids,
            pair<double,double> nearestAskWall,
            pair<double,double> nearestBidWall) {

    erase();
    int row = 0;
    char buf[256];

    printAt(row++, 0, COL_HEADER, "=============== PULSE ===============", true);
    snprintf(buf, sizeof(buf), "Price: $%.2f   Spread: $%.2f   Latency: %.0f ns   Rows: %lld",
             midPrice, spread, latencyNs, updateCount);
    printAt(row++, 0, COL_NEUTRAL, string(buf));
    snprintf(buf, sizeof(buf), "Updates: %d", numUpdates);
    printAt(row++, 0, COL_NEUTRAL, string(buf));
    row++;

    printAt(row++, 0, COL_HEADER, "--------- ORDER BOOK ---------");
    for (auto it = topAsks.rbegin(); it != topAsks.rend(); ++it) {
        snprintf(buf, sizeof(buf), "  $%.2f  |  %.4f BTC  <-- SELL", it->first, it->second);
        printAt(row++, 0, COL_ASK, string(buf));
    }
    snprintf(buf, sizeof(buf), "  ------- SPREAD: $%.2f -------", spread);
    printAt(row++, 0, COL_SPREAD, string(buf));
    for (auto& b : topBids) {
        snprintf(buf, sizeof(buf), "  $%.2f  |  %.4f BTC  <-- BUY ", b.first, b.second);
        printAt(row++, 0, COL_BID, string(buf));
    }
    row++;

    printAt(row++, 0, COL_HEADER, "--------- IMBALANCE ----------");
    string imbalanceStr;
    int imbalanceColor;
    if (imbalance > 0.6) {
        imbalanceStr = "BUY PRESSURE  >>>>";
        imbalanceColor = COL_BID;
    } else if (imbalance < 0.4) {
        imbalanceStr = "<<<< SELL PRESSURE";
        imbalanceColor = COL_ASK;
    } else {
        imbalanceStr = "      NEUTRAL      ";
        imbalanceColor = COL_NEUTRAL;
    }
    snprintf(buf, sizeof(buf), "  Imbalance: %.4f  %s", imbalance, imbalanceStr.c_str());
    printAt(row++, 0, imbalanceColor, string(buf), true);
    row++;

    printAt(row++, 0, COL_HEADER, "--------- FLOW TOXICITY ------");
    snprintf(buf, sizeof(buf), "  Buy  aggression: %.4f BTC", buyAggression);
    printAt(row++, 0, COL_BID, string(buf));
    snprintf(buf, sizeof(buf), "  Sell aggression: %.4f BTC", sellAggression);
    printAt(row++, 0, COL_ASK, string(buf));
    string toxStr;
    int toxColor;
    if      (aggressionRatio > 0.65) { toxStr = "AGGRESSIVE BUYERS";  toxColor = COL_BID; }
    else if (aggressionRatio < 0.35) { toxStr = "AGGRESSIVE SELLERS"; toxColor = COL_ASK; }
    else                              { toxStr = "MIXED AGGRESSION";   toxColor = COL_NEUTRAL; }
    snprintf(buf, sizeof(buf), "  Ratio: %.4f  %s", aggressionRatio, toxStr.c_str());
    printAt(row++, 0, toxColor, string(buf), true);
    row++;

    printAt(row++, 0, COL_HEADER, "--------- NEAREST WALLS ------");
    if (nearestAskWall.first > 0) {
        snprintf(buf, sizeof(buf), "  ASK WALL  $%.2f  |  %.2f BTC  ($%.2f away)",
                 nearestAskWall.first, nearestAskWall.second,
                 nearestAskWall.first - midPrice);
        printAt(row++, 0, COL_ASK, string(buf));
    } else {
        printAt(row++, 0, COL_NEUTRAL, "  ASK WALL  none nearby");
    }
    if (nearestBidWall.first > 0) {
        snprintf(buf, sizeof(buf), "  BID WALL  $%.2f  |  %.2f BTC  ($%.2f away)",
                 nearestBidWall.first, nearestBidWall.second,
                 midPrice - nearestBidWall.first);
        printAt(row++, 0, COL_BID, string(buf));
    } else {
        printAt(row++, 0, COL_NEUTRAL, "  BID WALL  none nearby");
    }
    row++;

    printAt(row++, 0, COL_HEADER, "--------- LAST ALERTS --------");
    if (recentWallEvents.empty()) {
        printAt(row++, 0, COL_NEUTRAL, "  none yet");
    } else {
        for (auto& e : recentWallEvents)
            printAt(row++, 0, COL_ALERT, "  " + e, true);
    }
    row++;

    drawImbalanceGraph(row);
    row += 12;
    drawLatencyGraph(row);

    refresh();
}

void renderOrderBook(double latencyNs, int numUpdates) {
    updateCount++;

    // ===== FIX: correct stale best indices before scanning =====
    while (bestBidIdx > 0 && bidLadder[bestBidIdx] == 0.0) bestBidIdx--;
    while (bestAskIdx < LADDER_SIZE - 1 && askLadder[bestAskIdx] == 0.0) bestAskIdx++;

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

    imbalanceHistory.push_back(imbalance);
    if ((int)imbalanceHistory.size() > IMBALANCE_HISTORY)
        imbalanceHistory.pop_front();

    latencyHistory.push_back(latencyNs);
    if ((int)latencyHistory.size() > LATENCY_HISTORY)
        latencyHistory.pop_front();

    double totalAggressive = totalAggressiveBuy + totalAggressiveSell;
    double buyAggression   = totalAggressiveBuy;
    double sellAggression  = totalAggressiveSell;
    double aggressionRatio = (totalAggressive > 0)
                             ? buyAggression / totalAggressive : 0.5;

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

    logToCSV(midPrice, bestBid, bestAsk, spread, latencyNs, numUpdates,
             imbalance, buyAggression, sellAggression, aggressionRatio,
             nearestAskWall.first, nearestAskWall.second,
             nearestBidWall.first, nearestBidWall.second);

    drawUI(midPrice, bestBid, bestAsk, spread, latencyNs, numUpdates,
           imbalance, buyAggression, sellAggression, aggressionRatio,
           topAsks, topBids, nearestAskWall, nearestBidWall);
}

int main() {
    initCSV();
    initNcurses();

    uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    while (true) {
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
                renderOrderBook(latencyNs, numUpdates);
            }
        }
        catch (exception const& e) {
            endwin();
            cerr << "Disconnected: " << e.what() << endl;
            cerr << "Reconnecting in 2 seconds..." << endl;
            this_thread::sleep_for(chrono::seconds(2));
            initNcurses();
        }
    }

    endwin();
    csvFile.close();
    return 0;
}