/*
 * Copyright The OpenTelemetry Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Windows.h>
#endif

#include "opentelemetry/logs/provider.h"

#include "opentelemetry/logs/provider.h"
#include "opentelemetry/sdk/logs/logger_provider.h"
#include "opentelemetry/sdk/logs/recordable.h"
#include "opentelemetry/sdk/logs/simple_log_processor.h"

#include "opentelemetry/sdk/logs/exporter.h"

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/exporters/fluentd/log/fluentd_exporter.h"
#include "opentelemetry/exporters/fluentd/log/recordable.h"

#include <iostream>

#include <map>
#include <string>

#include <gtest/gtest.h>

#include "../common/socket_server.h"

using namespace SOCKET_SERVER_NS;

using namespace opentelemetry::sdk::logs;
using namespace opentelemetry::sdk::resource;

namespace logs = opentelemetry::logs;
namespace nostd = opentelemetry::nostd;
namespace sdklogs = opentelemetry::sdk::logs;
using json = nlohmann::json;

#include <chrono>
#include <iostream>
#include <thread>

// "busy sleep" while suggesting that other threads run
// for a small amount of time
template <typename timeunit> void yield_for(timeunit duration) {
  auto start = std::chrono::high_resolution_clock::now();
  auto end = start + duration;
  do {
    std::this_thread::yield();
  } while (std::chrono::high_resolution_clock::now() < end);
}

using Properties = std::map<std::string, opentelemetry::common::AttributeValue>;

struct TestServer {
  SocketServer &server;
  std::atomic<uint32_t> count{0};

  TestServer(SocketServer &server) : server(server) {
    server.onRequest = [&](SocketServer::Connection &conn) {
      std::vector<uint8_t> msg(conn.request_buffer.data(),
                               conn.request_buffer.data() +
                                   conn.request_buffer.size());

      try {
        auto j = nlohmann::json::from_msgpack(msg);
        std::cout << "[" << count.fetch_add(1)
                  << "] SocketServer received payload: " << std::endl
                  << j.dump(2) << std::endl;

        conn.response_buffer = j.dump(2);
        conn.state.insert(SocketServer::Connection::Responding);
        conn.request_buffer.clear();
      } catch (std::exception &) {
        conn.state.insert(SocketServer::Connection::Receiving);
        // skip invalid payload
      }
    };
  }

  void Start() { server.Start(); }

  void Stop() { server.Stop(); }

  void WaitForEvents(uint32_t expectedCount, uint32_t timeout) {
    if (count.load() != expectedCount) {
      yield_for(std::chrono::milliseconds(timeout));
    }
    EXPECT_EQ(count.load(), expectedCount);
  }
};

TEST(FluentdExporter, SendLogEvents) {
  bool isRunning = true;

  // Start test server
  SocketAddr destination("127.0.0.1:24222");
  SocketParams params{AF_INET, SOCK_STREAM, 0};
  SocketServer socketServer(destination, params);
  TestServer testServer(socketServer);
  testServer.Start();

  yield_for(std::chrono::milliseconds(500));

  // Connect to local test server
  opentelemetry::exporter::fluentd::common::FluentdExporterOptions options;
  options.endpoint = "tcp://127.0.0.1:24222";
  options.tag = "tag.my_service";

  auto exporter = std::unique_ptr<sdklogs::LogExporter>(
      new opentelemetry::exporter::fluentd::logs::FluentdExporter(options));
  auto processor = std::shared_ptr<sdklogs::LogProcessor>(
      new sdklogs::SimpleLogProcessor(std::move(exporter)));

  auto provider = std::shared_ptr<opentelemetry::logs::LoggerProvider>(
      new opentelemetry::sdk::logs::LoggerProvider());

  auto pr =
      static_cast<opentelemetry::sdk::logs::LoggerProvider *>(provider.get());

  pr->SetProcessor(processor);

  // Set the global trace provider
  opentelemetry::logs::Provider::SetLoggerProvider(provider);

  std::string providerName = "MyInstrumentationName";
  auto logger = provider->GetLogger(providerName);

  // Span attributes
  Properties attribs = {{"attrib1", 1}, {"attrib2", 2}};

  logger->Log(logs::Severity::kDebug, "f2");
  logger->Log(logs::Severity::kDebug, "f3");

  testServer.WaitForEvents(2, 200); // 2 batches must arrive in 200ms
  testServer.Stop();
}
