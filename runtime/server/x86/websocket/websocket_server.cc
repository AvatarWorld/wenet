// Copyright (c) 2020 Mobvoi Inc (Binbin Zhang)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "websocket/websocket_server.h"

#include <thread>
#include <utility>
#include <vector>

#include "boost/json/src.hpp"
#include "glog/logging.h"

namespace wenet {

namespace beast = boost::beast;          // from <boost/beast.hpp>
namespace http = beast::http;            // from <boost/beast/http.hpp>
namespace websocket = beast::websocket;  // from <boost/beast/websocket.hpp>
namespace asio = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;        // from <boost/asio/ip/tcp.hpp>
namespace json = boost::json;

ConnectionHandler::ConnectionHandler(tcp::socket&& socket,
                    std::shared_ptr<FeaturePipelineConfig> feature_config,
                    std::shared_ptr<DecodeOptions> decode_config,
                    std::shared_ptr<SymbolTable> symbol_table,
                    std::shared_ptr<TorchAsrModel> model)
      : ws_(std::move(socket)),
        feature_config_(feature_config),
        decode_config_(decode_config),
        symbol_table_(symbol_table),
        model_(model) {}

ConnectionHandler::ConnectionHandler(ConnectionHandler&& other)
      : ws_(std::move(other.ws_)),
        feature_config_(other.feature_config_),
        decode_config_(other.decode_config_),
        symbol_table_(other.symbol_table_),
        model_(other.model_) {}

void ConnectionHandler::OnSpeechStart() {
  LOG(INFO) << "Recieved speech start signal, start reading speech";
  got_start_tag_ = true;
  json::value rv = {{"status", "ok"}, {"type", "server_ready"}};
  ws_.text(true);
  ws_.write(asio::buffer(json::serialize(rv)));
  feature_pipeline_ = std::make_shared<FeaturePipeline>(*feature_config_);
  decoder_ = std::make_shared<TorchAsrDecoder>(feature_pipeline_, model_,
                                               *symbol_table_, *decode_config_);
  // Start decoder thread
  decode_thread_ =
      std::make_shared<std::thread>(&ConnectionHandler::DecodeThreadFunc, this);
}

void ConnectionHandler::OnSpeechEnd() {
  LOG(INFO) << "Recieved speech end signal";
  CHECK(feature_pipeline_ != nullptr);
  feature_pipeline_->set_input_finished();
}

void ConnectionHandler::OnPartialResult(const std::string& result) {
  LOG(INFO) << "Partial result: " << result;
  json::value rv = {
      {"status", "ok"}, {"type", "partial_result"}, {"content", result}};
  ws_.text(true);
  ws_.write(asio::buffer(json::serialize(rv)));
}

void ConnectionHandler::OnFinalResult(const std::string& result) {
  LOG(INFO) << "Final result: " << result;
  json::value rv = {
      {"status", "ok"}, {"type", "final_result"}, {"content", result}};
  ws_.text(true);
  ws_.write(asio::buffer(json::serialize(rv)));

  // Send finish tag
  json::value rv2 = {{"status", "ok"}, {"type", "speech_end"}};
  ws_.text(true);
  ws_.write(asio::buffer(json::serialize(rv2)));
}

void ConnectionHandler::OnSpeechData(const beast::flat_buffer& buffer) {
  // Read binary PCM data
  int num_samples = buffer.size() / sizeof(int16_t);
  std::vector<float> pcm_data(num_samples);
  const int16_t* pdata = static_cast<const int16_t*>(buffer.data().data());
  for (int i = 0; i < num_samples; i++) {
    pcm_data[i] = static_cast<float>(*pdata);
    pdata++;
  }
  VLOG(2) << "Recieved " << num_samples << " samples";
  CHECK(feature_pipeline_ != nullptr);
  CHECK(decoder_ != nullptr);
  feature_pipeline_->AcceptWaveform(pcm_data);
}

void ConnectionHandler::DecodeThreadFunc() {
  while (true) {
    bool finish = decoder_->Decode();
    const std::string& result = decoder_->result();
    if (finish) {
      OnFinalResult(result);
      break;
    } else {
      OnPartialResult(result);
    }
  }
}

void ConnectionHandler::OnError(const std::string& message) {
  json::value rv = {{"status", "failed"}, {"message", message}};
  ws_.text(true);
  ws_.write(asio::buffer(json::serialize(rv)));
  // Close websocket
  ws_.close(websocket::close_code::normal);
}

void ConnectionHandler::operator()() {
  try {
    // Accept the websocket handshake
    ws_.accept();
    for (;;) {
      // This buffer will hold the incoming message
      beast::flat_buffer buffer;
      // Read a message
      ws_.read(buffer);
      if (ws_.got_text()) {
        std::string message = beast::buffers_to_string(buffer.data());
        LOG(INFO) << message;
        json::value v = json::parse(message);
        if (v.is_object()) {
          json::object obj = v.get_object();
          if (obj.find("signal") != obj.end()) {
            json::string signal = obj["signal"].as_string();
            if (signal == "start") {
              OnSpeechStart();
            } else if (signal == "end") {
              OnSpeechEnd();
              break;
            } else {
              OnError("Unexpected signal type");
            }
          } else {
            OnError("Wrong message header");
          }
        }
      } else {
        if (!got_start_tag_) {
          OnError("Start singal is expected before binary data");
        } else {
          OnSpeechData(buffer);
        }
      }
    }

    LOG(INFO) << "Read all pcm data, wait for decoding thread";
    if (decode_thread_ != nullptr) {
      decode_thread_->join();
    }
  } catch (beast::system_error const& se) {
    // This indicates that the session was closed
    if (se.code() != websocket::error::closed) {
      LOG(ERROR) << se.code().message();
    }
  } catch (std::exception const& e) {
    LOG(ERROR) << e.what();
  }
}

void WebSocketServer::Start() {
  try {
    auto const address = asio::ip::make_address("0.0.0.0");
    tcp::acceptor acceptor{ioc_, {address, static_cast<uint16_t>(port_)}};
    for (;;) {
      // This will receive the new connection
      tcp::socket socket{ioc_};
      // Block until we get a connection
      acceptor.accept(socket);
      // Launch the session, transferring ownership of the socket
      ConnectionHandler handler(std::move(socket), feature_config_,
                                decode_config_, symbol_table_, model_);
      std::thread t(std::move(handler));
      t.detach();
    }
  } catch (const std::exception& e) {
    LOG(FATAL) << e.what();
  }
}

}  // namespace wenet
