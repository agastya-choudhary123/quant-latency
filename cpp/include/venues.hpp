// Venue adapters: real subscribe messages + parsers translating each exchange's
// native JSON into the common MarketUpdate. One connection may carry several
// channels (OKX carries tickers + funding-rate), dispatched inside the parser.
#pragma once
#include "feed.hpp"
#include <string_view>
#include <cctype>

namespace fa {

// ---- OKX (perp): top-of-book from `tickers` + `funding-rate` -------------
inline VenueSpec okx_perp_spec(const std::string& inst /* BTC-USDT-SWAP */) {
    VenueSpec v;
    v.venue = "okx"; v.host = "ws.okx.com"; v.port = "8443"; v.path = "/ws/v5/public";
    v.subscribes = {
        R"({"op":"subscribe","args":[{"channel":"tickers","instId":")" + inst + R"("}]})",
        R"({"op":"subscribe","args":[{"channel":"funding-rate","instId":")" + inst + R"("}]})",
    };
    v.parser = [](std::string_view msg, uint64_t recv_ns,
                  const std::function<void(const MarketUpdate&)>& emit) -> int {
        if (msg.find("\"data\"") == std::string_view::npos) return 0;  // ack/event
        std::string inst_id;
        json_string(msg, "instId", inst_id);
        if (msg.find("\"funding-rate\"") != std::string_view::npos ||
            msg.find("\"fundingRate\"") != std::string_view::npos) {
            double fr = 0;
            if (!json_number(msg, "fundingRate", fr)) return 0;
            MarketUpdate u; set_cstr(u.venue,sizeof u.venue,"okx");
            set_cstr(u.symbol,sizeof u.symbol,inst_id);
            u.funding_rate = fr; u.recv_ns = recv_ns; u.kind = UpdKind::Funding;
            emit(u); return 1;
        }
        double bid=0, ask=0, last=0, bsz=0, asz=0;
        bool have_bid = json_number(msg,"bidPx",bid);
        bool have_ask = json_number(msg,"askPx",ask);
        json_number(msg,"last",last);
        json_number(msg,"bidSz",bsz);
        json_number(msg,"askSz",asz);
        if (!have_bid || !have_ask) return 0;
        MarketUpdate u; set_cstr(u.venue,sizeof u.venue,"okx");
        set_cstr(u.symbol,sizeof u.symbol,inst_id);
        u.bid=bid; u.ask=ask; u.last=last; u.bid_sz=bsz; u.ask_sz=asz;
        u.recv_ns=recv_ns; u.kind=UpdKind::Book;
        emit(u); return 1;
    };
    return v;
}

// ---- Coinbase (spot): `ticker` channel -----------------------------------
inline VenueSpec coinbase_spot_spec(const std::string& product /* BTC-USD */) {
    VenueSpec v;
    v.venue="coinbase"; v.host="advanced-trade-ws.coinbase.com"; v.port="443"; v.path="/";
    v.subscribes = {
        R"({"type":"subscribe","product_ids":[")" + product +
        R"("],"channel":"ticker"})",
    };
    v.parser = [](std::string_view msg, uint64_t recv_ns,
                  const std::function<void(const MarketUpdate&)>& emit) -> int {
        if (msg.find("\"ticker\"") == std::string_view::npos) return 0;
        double price=0, bid=0, ask=0, bsz=0, asz=0;
        if (!json_number(msg,"price",price)) return 0;
        json_number(msg,"best_bid",bid);
        json_number(msg,"best_ask",ask);
        json_number(msg,"best_bid_quantity",bsz);
        json_number(msg,"best_ask_quantity",asz);
        std::string pid; json_string(msg,"product_id",pid);
        MarketUpdate u; set_cstr(u.venue,sizeof u.venue,"coinbase");
        set_cstr(u.symbol,sizeof u.symbol,pid);
        u.bid=bid; u.ask=ask; u.last=price; u.bid_sz=bsz; u.ask_sz=asz;
        u.recv_ns=recv_ns; u.kind=UpdKind::Book;
        emit(u); return 1;
    };
    return v;
}

// ---- Kraken (spot): v2 `ticker` channel (bare numeric fields) -------------
inline VenueSpec kraken_spot_spec(const std::string& pair /* BTC/USD */) {
    VenueSpec v;
    v.venue="kraken"; v.host="ws.kraken.com"; v.port="443"; v.path="/v2";
    v.subscribes = {
        R"({"method":"subscribe","params":{"channel":"ticker","symbol":[")" +
        pair + R"("]}})",
    };
    v.parser = [](std::string_view msg, uint64_t recv_ns,
                  const std::function<void(const MarketUpdate&)>& emit) -> int {
        if (msg.find("\"ticker\"") == std::string_view::npos) return 0;
        if (msg.find("\"data\"") == std::string_view::npos) return 0;
        double bid=0, ask=0, last=0, bsz=0, asz=0;
        bool have_bid = json_number(msg,"bid",bid);
        bool have_ask = json_number(msg,"ask",ask);
        json_number(msg,"last",last);
        json_number(msg,"bid_qty",bsz);
        json_number(msg,"ask_qty",asz);
        if (!have_bid || !have_ask) return 0;
        std::string sym; json_string(msg,"symbol",sym);
        MarketUpdate u; set_cstr(u.venue,sizeof u.venue,"kraken");
        set_cstr(u.symbol,sizeof u.symbol,sym);
        u.bid=bid; u.ask=ask; u.last=last; u.bid_sz=bsz; u.ask_sz=asz;
        u.recv_ns=recv_ns; u.kind=UpdKind::Book;
        emit(u); return 1;
    };
    return v;
}

// ---- Binance (USD-M perp): combined stream, bookTicker + markPrice --------
// One socket carries two streams via the /stream?streams=a/b multiplexer. Each
// frame is wrapped as {"stream":"...","data":{...}}. bookTicker gives real-time
// top of book (single-letter keys b/B/a/A); markPrice carries the funding rate
// (r) on its 1s cadence. Symbol arrives lower-case in the stream name but as an
// upper-case `s` inside data, which is what we key on.
inline VenueSpec binance_perp_spec(const std::string& sym /* BTCUSDT */) {
    std::string lower;
    for (char c : sym) lower += char(std::tolower((unsigned char)c));
    VenueSpec v;
    v.venue = "binance"; v.host = "fstream.binance.com"; v.port = "443";
    v.path = "/stream?streams=" + lower + "@bookTicker/" + lower + "@markPrice";
    // Binance pushes the combined stream on connect; no explicit subscribe frame
    // is required for URL-embedded streams. Send none.
    v.subscribes = {};
    v.parser = [](std::string_view msg, uint64_t recv_ns,
                  const std::function<void(const MarketUpdate&)>& emit) -> int {
        // markPrice frame: funding rate in "r".
        if (msg.find("\"markPriceUpdate\"") != std::string_view::npos) {
            double fr = 0;
            if (!json_number(msg, "r", fr)) return 0;
            std::string sym_id; json_string(msg, "s", sym_id);
            MarketUpdate u; set_cstr(u.venue,sizeof u.venue,"binance");
            set_cstr(u.symbol,sizeof u.symbol,sym_id);
            u.funding_rate = fr; u.recv_ns = recv_ns; u.kind = UpdKind::Funding;
            emit(u); return 1;
        }
        // bookTicker frame: b/B/a/A = bidPx/bidSz/askPx/askSz.
        if (msg.find("\"bookTicker\"") == std::string_view::npos) return 0;
        double bid=0, ask=0, bsz=0, asz=0;
        bool have_bid = json_number(msg,"b",bid);
        bool have_ask = json_number(msg,"a",ask);
        json_number(msg,"B",bsz);
        json_number(msg,"A",asz);
        if (!have_bid || !have_ask) return 0;
        std::string sym_id; json_string(msg,"s",sym_id);
        MarketUpdate u; set_cstr(u.venue,sizeof u.venue,"binance");
        set_cstr(u.symbol,sizeof u.symbol,sym_id);
        u.bid=bid; u.ask=ask; u.last=0.5*(bid+ask); u.bid_sz=bsz; u.ask_sz=asz;
        u.recv_ns=recv_ns; u.kind=UpdKind::Book;
        emit(u); return 1;
    };
    return v;
}

} // namespace fa
