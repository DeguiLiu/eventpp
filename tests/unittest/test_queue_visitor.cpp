// eventpp library
// Copyright (C) 2018 Wang Qi (wqking)
// Github: https://github.com/wqking/eventpp
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//   http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// OPT-15: Tests for processQueueWith / processOneWith
// Zero-overhead visitor dispatch that bypasses the full EventDispatcher chain.

#include "test.h"
#include "eventpp/eventqueue.h"

#include <string>
#include <vector>

TEST_CASE("processQueueWith, basic dispatch")
{
	eventpp::EventQueue<int, void (int, const std::string &)> queue;

	int receivedEvent = -1;
	int receivedArg = -1;
	std::string receivedStr;

	queue.enqueue(42, 100, std::string("hello"));

	auto visitor = [&](int event, int arg, const std::string & s) {
		receivedEvent = event;
		receivedArg = arg;
		receivedStr = s;
	};

	REQUIRE(queue.processQueueWith(visitor));
	REQUIRE(receivedEvent == 42);
	REQUIRE(receivedArg == 100);
	REQUIRE(receivedStr == "hello");
}

TEST_CASE("processQueueWith, processes all events")
{
	eventpp::EventQueue<int, void ()> queue;

	int count = 0;

	queue.enqueue(1);
	queue.enqueue(2);
	queue.enqueue(3);
	queue.enqueue(4);
	queue.enqueue(5);

	REQUIRE(queue.processQueueWith([&](int /*event*/) {
		++count;
	}));

	REQUIRE(count == 5);
}

TEST_CASE("processQueueWith, returns false on empty queue")
{
	eventpp::EventQueue<int, void ()> queue;

	REQUIRE(! queue.processQueueWith([](int /*event*/) {}));
}

TEST_CASE("processQueueWith, event order preserved")
{
	eventpp::EventQueue<int, void ()> queue;

	std::vector<int> order;

	queue.enqueue(10);
	queue.enqueue(20);
	queue.enqueue(30);
	queue.enqueue(40);

	queue.processQueueWith([&](int event) {
		order.push_back(event);
	});

	REQUIRE(order.size() == 4);
	REQUIRE(order[0] == 10);
	REQUIRE(order[1] == 20);
	REQUIRE(order[2] == 30);
	REQUIRE(order[3] == 40);
}

TEST_CASE("processOneWith, basic dispatch")
{
	eventpp::EventQueue<int, void (int)> queue;

	int receivedEvent = -1;
	int receivedArg = -1;

	queue.enqueue(5, 99);

	REQUIRE(queue.processOneWith([&](int event, int arg) {
		receivedEvent = event;
		receivedArg = arg;
	}));

	REQUIRE(receivedEvent == 5);
	REQUIRE(receivedArg == 99);
}

TEST_CASE("processOneWith, leaves remaining events")
{
	eventpp::EventQueue<int, void ()> queue;

	int count = 0;

	queue.enqueue(1);
	queue.enqueue(2);
	queue.enqueue(3);

	REQUIRE(queue.processOneWith([&](int /*event*/) {
		++count;
	}));
	REQUIRE(count == 1);

	// Remaining events should still be in the queue
	REQUIRE(queue.processOneWith([&](int /*event*/) {
		++count;
	}));
	REQUIRE(count == 2);

	REQUIRE(queue.processOneWith([&](int /*event*/) {
		++count;
	}));
	REQUIRE(count == 3);

	// Queue should be empty now
	REQUIRE(! queue.processOneWith([&](int /*event*/) {
		++count;
	}));
	REQUIRE(count == 3);
}

TEST_CASE("processQueueWith, with SingleThreading policy")
{
	struct SingleThreadPolicies {
		using Threading = eventpp::SingleThreading;
	};

	eventpp::EventQueue<int, void (int), SingleThreadPolicies> queue;

	int sum = 0;

	queue.enqueue(1, 10);
	queue.enqueue(2, 20);
	queue.enqueue(3, 30);

	queue.processQueueWith([&](int /*event*/, int value) {
		sum += value;
	});

	REQUIRE(sum == 60);
}

TEST_CASE("processQueueWith vs process parity")
{
	// Test that processQueueWith and process receive the same data
	eventpp::EventQueue<int, void (int, const std::string &)> queue1;
	eventpp::EventQueue<int, void (int, const std::string &)> queue2;

	// Same events enqueued to both
	queue1.enqueue(1, 10, std::string("a"));
	queue1.enqueue(2, 20, std::string("b"));
	queue1.enqueue(3, 30, std::string("c"));

	queue2.enqueue(1, 10, std::string("a"));
	queue2.enqueue(2, 20, std::string("b"));
	queue2.enqueue(3, 30, std::string("c"));

	// Collect via processQueueWith
	std::vector<int> visitorEvents;
	std::vector<int> visitorArgs;
	std::vector<std::string> visitorStrings;

	queue1.processQueueWith([&](int event, int arg, const std::string & s) {
		visitorEvents.push_back(event);
		visitorArgs.push_back(arg);
		visitorStrings.push_back(s);
	});

	// Collect via process (appendListener)
	std::vector<int> listenerEvents;
	std::vector<int> listenerArgs;
	std::vector<std::string> listenerStrings;

	queue2.appendListener(1, [&](int arg, const std::string & s) {
		listenerEvents.push_back(1);
		listenerArgs.push_back(arg);
		listenerStrings.push_back(s);
	});
	queue2.appendListener(2, [&](int arg, const std::string & s) {
		listenerEvents.push_back(2);
		listenerArgs.push_back(arg);
		listenerStrings.push_back(s);
	});
	queue2.appendListener(3, [&](int arg, const std::string & s) {
		listenerEvents.push_back(3);
		listenerArgs.push_back(arg);
		listenerStrings.push_back(s);
	});

	queue2.process();

	// Both should have same results
	REQUIRE(visitorEvents == listenerEvents);
	REQUIRE(visitorArgs == listenerArgs);
	REQUIRE(visitorStrings == listenerStrings);
}

TEST_CASE("processQueueWith, with string event type")
{
	eventpp::EventQueue<std::string, void (const std::string &)> queue;

	std::vector<std::string> receivedEvents;
	std::vector<std::string> receivedArgs;

	queue.enqueue("event_a", std::string("data_a"));
	queue.enqueue("event_b", std::string("data_b"));

	queue.processQueueWith([&](const std::string & event, const std::string & arg) {
		receivedEvents.push_back(event);
		receivedArgs.push_back(arg);
	});

	REQUIRE(receivedEvents.size() == 2);
	REQUIRE(receivedEvents[0] == "event_a");
	REQUIRE(receivedEvents[1] == "event_b");
	REQUIRE(receivedArgs[0] == "data_a");
	REQUIRE(receivedArgs[1] == "data_b");
}

TEST_CASE("processQueueWith, complex arguments")
{
	struct Data {
		int x;
		float y;
		std::string name;
	};

	eventpp::EventQueue<int, void (const Data &, int)> queue;

	queue.enqueue(1, Data{10, 1.5f, "first"}, 100);
	queue.enqueue(2, Data{20, 2.5f, "second"}, 200);

	std::vector<int> events;
	std::vector<int> xs;
	std::vector<float> ys;
	std::vector<std::string> names;
	std::vector<int> extras;

	queue.processQueueWith([&](int event, const Data & d, int extra) {
		events.push_back(event);
		xs.push_back(d.x);
		ys.push_back(d.y);
		names.push_back(d.name);
		extras.push_back(extra);
	});

	REQUIRE(events.size() == 2);
	REQUIRE(events[0] == 1);
	REQUIRE(events[1] == 2);
	REQUIRE(xs[0] == 10);
	REQUIRE(xs[1] == 20);
	REQUIRE(names[0] == "first");
	REQUIRE(names[1] == "second");
	REQUIRE(extras[0] == 100);
	REQUIRE(extras[1] == 200);
}
