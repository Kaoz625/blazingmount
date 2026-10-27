/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * blazingmount.cpp - the BlazingMount app as a PS5 title (PPSA99280), on mihawk-99's PS5_VulkanTemplate (MIT) and its stack
 * (PS5_Vulkan, RADV from PS5_Mesa, PS5_PayloadSDK): the same stack and UI pass as Multiverse Fighters (blazing-fighters).
 *
 * The app itself is app/src/bm_app.h (every screen, the controller, what it asks); this file only:
 *   - reads pad 0 into presses (d-pad / left stick with key repeat: 0.40 s, then every 0.09 s),
 *   - runs the app's requests on ONE worker thread against blazingmountd's API at 127.0.0.1:8094 (bm_api.c) and hands the
 *     answers back on the render thread (a title cannot read /data: everything it shows comes from the payload),
 *   - draws the app's quad batch with the UI pass (ui_vk.cpp, one pipeline, the Barlow distance-field atlas).
 * CIRCLE on the Library with nothing open quits back to the home screen.
 * Logs go to klog with the [BlazingMount] prefix (nc <console> 3232).
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/bm_app.h"
#include "platform.h"
#include "ui_vk.h"

class VulkanExample : public VulkanExampleBase
{
	static constexpr const char *kVersion = "0.6.1";
	static constexpr int kApiPort = 8094;

	bm::App app;
	mf::ui::Ps5Ui ui;
	mf::ui::Font font;
	mf::ui::Batch batch;
	int64_t startNs = 0;

	// --- the pad: what was held last frame, and the repeat clock of the held direction
	uint32_t lastHeld = 0;
	int heldDir = -1;
	double dirSince = 0, dirNext = 0;

	// --- the worker: requests in, answers out
	struct Answer {
		std::string path;
		int code;
		std::string body;
	};
	std::mutex mu;
	std::condition_variable cv;
	std::deque<bm::Request> todo;
	std::deque<Answer> done;
	std::atomic<bool> stop{ false };
	std::thread worker;

	static int64_t steadyNs()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	double now() const { return (steadyNs() - startNs) / 1e9; }

	static std::string readFile(const std::string &path)
	{
		std::string out;
		FILE *f = fopen(path.c_str(), "rb");
		if (!f) {
			return out;
		}
		char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
			out.append(buf, n);
		}
		fclose(f);
		return out;
	}

	// This console's own LAN address (a UDP "connect" sends nothing; getsockname tells the route's source address)
	static uint32_t lanAddress()
	{
		int u = socket(AF_INET, SOCK_DGRAM, 0);
		if (u < 0) {
			return 0;
		}
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_port = htons(9);
		to.sin_addr.s_addr = htonl(0xC0A80101);  // 192.168.1.1: any address off the box picks the LAN route
		sockaddr_in me{};
		socklen_t ml = sizeof me;
		uint32_t ip = 0;
		if (connect(u, reinterpret_cast<sockaddr *>(&to), sizeof to) == 0 && getsockname(u, reinterpret_cast<sockaddr *>(&me), &ml) == 0) {
			ip = me.sin_addr.s_addr;
		}
		close(u);
		return ip;
	}

	// 127.0.0.1 first; when a title may not reach loopback, the console's own LAN address (said once in klog)
	static int connectApi()
	{
		static std::atomic<int> route{ 0 };   // 0 loopback, 1 LAN address
		for (int k = 0; k < 2; k++) {
			const int r = (route + k) % 2;
			const uint32_t ip = r == 0 ? htonl(INADDR_LOOPBACK) : lanAddress();
			if (!ip) {
				continue;
			}
			int s = socket(AF_INET, SOCK_STREAM, 0);
			if (s < 0) {
				return -1;
			}
			timeval tv{ 3, 0 };
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
			setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
			sockaddr_in sa{};
			sa.sin_family = AF_INET;
			sa.sin_port = htons(kApiPort);
			sa.sin_addr.s_addr = ip;
			if (connect(s, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0) {
				if (r != route) {
					route = r;
					say("api: reaching blazingmountd at %s:%d", r ? inet_ntoa(sa.sin_addr) : "127.0.0.1", kApiPort);
				}
				return s;
			}
			close(s);
		}
		return -1;
	}

	// One HTTP/1.0 request to the payload on this console. code 0 = nothing answered (the payload is not running).
	static Answer fetch(const bm::Request &r)
	{
		Answer a{ r.path, 0, "" };
		int s = connectApi();
		if (s < 0) {
			return a;
		}
		std::string req = r.method + " " + r.path + " HTTP/1.0\r\nHost: 127.0.0.1\r\nX-BM-Request: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		if (send(s, req.data(), req.size(), 0) != (ssize_t)req.size()) {
			close(s);
			return a;
		}
		std::string in;
		char buf[16384];
		for (;;) {
			ssize_t n = recv(s, buf, sizeof buf, 0);
			if (n <= 0) {
				break;
			}
			in.append(buf, size_t(n));
			if (in.size() > (8u << 20)) {
				break;
			}
		}
		close(s);
		size_t hdr = in.find("\r\n\r\n");
		if (in.compare(0, 7, "HTTP/1.") != 0 || hdr == std::string::npos) {
			return a;
		}
		a.code = atoi(in.c_str() + 9);
		a.body = in.substr(hdr + 4);
		return a;
	}

	void work()
	{
		while (!stop) {
			bm::Request r;
			{
				std::unique_lock<std::mutex> lk(mu);
				cv.wait_for(lk, std::chrono::milliseconds(200), [&] { return !todo.empty() || stop; });
				if (todo.empty()) {
					continue;
				}
				r = todo.front();
				todo.pop_front();
			}
			Answer a = fetch(r);
			std::lock_guard<std::mutex> lk(mu);
			done.push_back(std::move(a));
		}
	}

public:
	VulkanExample() : VulkanExampleBase()
	{
		title = "BlazingMount";
		name = "blazingmount";
		settings.overlay = false;
		ps5.ownOverlay = true;  // OPTIONS and TOUCH PAD are the app's
		settings.vsync = true;
		width = 1920;
		height = 1080;
		say("BlazingMount app %s: the library of blazingmountd (127.0.0.1:%d)", kVersion, kApiPort);
		const std::string fontBytes = readFile(getAssetPath() + "ui/bm-font.sdf");
		if (!mf::ui::LoadFont(reinterpret_cast<const uint8_t *>(fontBytes.data()), fontBytes.size(), font)) {
			say("ui: could not read %sui/bm-font.sdf (%zu bytes) - no text", getAssetPath().c_str(), fontBytes.size());
		}
		startNs = steadyNs();
		worker = std::thread([this] { work(); });
	}

	~VulkanExample()
	{
		stop = true;
		cv.notify_all();
		if (worker.joinable()) {
			worker.join();
		}
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		prepared = true;
		say("display %ux%u, swapchain format %d", width, height, (int)swapChain.colorFormat);
	}

	void render() override
	{
		if (!prepared) {
			return;
		}
		const double t = now();
		readPad(t);
		// answers in, requests out
		std::deque<Answer> answers;
		{
			std::lock_guard<std::mutex> lk(mu);
			answers.swap(done);
		}
		for (auto &a : answers) {
			if (a.path.rfind("/api/action", 0) == 0) {
				say("action %s -> http %d %s", a.path.c_str(), a.code, a.body.substr(0, 160).c_str());
			}
			app.OnResponse(a.path, a.code, a.body);
		}
		app.Tick(t);
		auto reqs = app.TakeRequests();
		if (!reqs.empty()) {
			std::lock_guard<std::mutex> lk(mu);
			for (auto &r : reqs) {
				todo.push_back(r);
			}
			cv.notify_one();
		}
		VulkanExampleBase::prepareFrame();
		buildCommandBuffer();
		VulkanExampleBase::submitFrame();
	}

private:
	void press(bm::Button b)
	{
		if (b == bm::kCircle && (app.tab() == bm::Tab::kLibrary || app.tab() == bm::Tab::kDiscover) && !app.confirming()) {
			say("CIRCLE on the Library or Discover: quit");
			quit = true;
			return;
		}
		app.Press(b);
	}

	void readPad(double t)
	{
		struct pad p {};
		const bool test = ps5.frameBudget != 0;
		if (test || !(pad_players() & 1u) || !pad_player(0, &p)) {
			lastHeld = 0;
			heldDir = -1;
			return;
		}
		uint32_t held = p.held;
		// the left stick as a d-pad
		if (p.left_x < -0.5f) held |= PAD_LEFT;
		if (p.left_x > 0.5f) held |= PAD_RIGHT;
		if (p.left_y < -0.5f) held |= PAD_UP;
		if (p.left_y > 0.5f) held |= PAD_DOWN;
		uint32_t l2 = p.l2 > 0.5f ? 1u << 28 : 0, r2 = p.r2 > 0.5f ? 1u << 29 : 0;
		held |= l2 | r2;
		const uint32_t pressed = held & ~lastHeld;
		lastHeld = held;
		static const struct {
			uint32_t mask;
			bm::Button b;
		} buttons[] = { { PAD_CROSS, bm::kCross }, { PAD_CIRCLE, bm::kCircle }, { PAD_SQUARE, bm::kSquare },
				{ PAD_TRIANGLE, bm::kTriangle }, { PAD_L1, bm::kL1 }, { PAD_R1, bm::kR1 },
				{ 1u << 28, bm::kL2 }, { 1u << 29, bm::kR2 }, { PAD_OPTIONS, bm::kOptions },
				{ PAD_TOUCH_PAD, bm::kTouch } };
		for (auto &e : buttons) {
			if (pressed & e.mask) {
				press(e.b);
			}
		}
		// directions: one press, then repeat while held
		static const struct {
			uint32_t mask;
			bm::Button b;
		} dirs[] = { { PAD_UP, bm::kUp }, { PAD_DOWN, bm::kDown }, { PAD_LEFT, bm::kLeft }, { PAD_RIGHT, bm::kRight } };
		int now = -1;
		for (int i = 0; i < 4; i++) {
			if (held & dirs[i].mask) {
				now = i;
				break;
			}
		}
		if (now < 0) {
			heldDir = -1;
		} else if (now != heldDir) {
			heldDir = now;
			dirSince = t;
			dirNext = t + 0.40;
			press(dirs[now].b);
		} else if (t >= dirNext) {
			dirNext = t + 0.09;
			press(dirs[now].b);
		}
	}

	void buildCommandBuffer()
	{
		VkCommandBuffer cmd = drawCmdBuffers[currentBuffer];
		const VkCommandBufferBeginInfo beginInfo = vks::initializers::commandBufferBeginInfo();
		VK_CHECK_RESULT(vkBeginCommandBuffer(cmd, &beginInfo));
		VkClearValue clearValues[2]{};
		clearValues[0].color = { { 10 / 255.0f, 10 / 255.0f, 11 / 255.0f, 1.0f } };
		clearValues[1].depthStencil = { 1.0f, 0 };
		VkRenderPassBeginInfo rp = vks::initializers::renderPassBeginInfo();
		rp.renderPass = renderPass;
		rp.framebuffer = frameBuffers[currentImageIndex];
		rp.renderArea.extent = { width, height };
		rp.clearValueCount = 2;
		rp.pClearValues = clearValues;
		vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
		batch.Reset(&font, mf::ui::PadStyle::kPlayStation);
		app.Draw(batch, now());
		bm::Covers &cv = app.covers();
		const mf::ui::ImageSource covers{ cv.rgba.data(), bm::Covers::kSize, bm::Covers::kSize, cv.version };
		ui.Draw(cmd, mf::ui::Ps5UiTarget{ vulkanDevice, queue, renderPass, pipelineCache, swapChain.colorFormat, width, height, currentBuffer },
			font, batch.vertices, &covers);
		vkCmdEndRenderPass(cmd);
		VK_CHECK_RESULT(vkEndCommandBuffer(cmd));
	}
};

VULKAN_EXAMPLE_MAIN()
