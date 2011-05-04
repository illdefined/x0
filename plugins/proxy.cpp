/* <x0/plugins/proxy.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#include <x0/http/HttpPlugin.h>
#include <x0/http/HttpServer.h>
#include <x0/http/HttpRequest.h>
#include <x0/io/BufferSource.h>
#include <x0/strutils.h>
#include <x0/Url.h>
#include <x0/Types.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>

/* {{{ -- configuration proposal:
 *
 * handler setup {
 * }
 *
 * handler main {
 *     proxy.reverse 'http://127.0.0.1:3000';
 * }
 *
 * --------------------------------------------------------------------------
 * possible tweaks:
 *  - bufsize (0 = unbuffered)
 *  - timeout.connect
 *  - timeout.write
 *  - timeout.read
 *  - ignore_clientabort
 * };
 *
 *
 */ // }}}

#if 1
#	define TRACE(msg...) DEBUG("proxy: " msg)
#else
#	define TRACE(msg...) /*!*/
#endif

enum class ConnectResult {
	Failed,
	Established,
	InProgress
};


// {{{ ProxyConnection API
class ProxyConnection :
	public x0::HttpMessageProcessor
{
private:
	enum State {
		DISCONNECTED,
		ABOUT_TO_CONNECT,
		CONNECTING,
		CONNECTED,
		WRITING,
		READING
	};

private:
	char *hostname_;				//!< origin's hostname
	int port_;						//!< origin's port
	int fd_;						//!< origin's socket fd
	x0::HttpRequest *request_;		//!< client's request

	State state_;

	ev::io io_;
	ev::timer timer_;
	int connectTimeout_;
	int readTimeout_;
	int writeTimeout_;

	x0::Buffer writeBuffer_;
	size_t writeOffset_;
	size_t writeProgress_;
	bool writeActive_;

	x0::Buffer readBuffer_;
	bool readActive_;

	// tweaks
	bool cloak_;

	const char *state_str() const {
		switch (state_) {
			case DISCONNECTED: return "DISCONNECTED";
			case ABOUT_TO_CONNECT: return "ABOUT_TO_CONNECT";
			case CONNECTING: return "CONNECTING";
			case CONNECTED: return "CONNECTED";
			case WRITING: return "WRITING";
			case READING: return "READING";
			default: return "INVALID!";
		}
	}

private:
	bool writeActive() const { return writeActive_; }
	bool readActive() const { return readActive_; }
	bool isReading() const { return state_ == READING; }
	bool isWriting() const { return state_ == WRITING; }
	bool isClosed() const { return fd_ < 0; }

	inline void close();
	inline void destroy(x0::HttpError code = x0::HttpError::Undefined);

	inline bool connect();
	inline ConnectResult openUnix(const std::string& unixPath, int* out_fd);
	inline ConnectResult openTcp(const std::string& hostname, int port, int* out_fd);
	inline void startRead();
	inline void startWrite();

	inline void onConnectComplete();
	inline void readSome();
	inline void writeSome();

	void io(ev::io& w, int revents);
	void timeout(ev::timer& w, int revents);
	void onRequestChunk(x0::BufferRef&& chunk);

	static void onAbort(void *p);

	// response (HttpMessageProcessor)
	virtual void messageBegin(int version_major, int version_minor, int code, x0::BufferRef&& text);
	virtual void messageHeader(x0::BufferRef&& name, x0::BufferRef&& value);
	virtual bool messageContent(x0::BufferRef&& chunk);
	virtual bool messageEnd();

public:
	inline ProxyConnection(const char *origin, x0::HttpRequest *r, bool cloak = true);
	~ProxyConnection();

	inline void start();
};
// }}}

// {{{ ProxyConnection impl
ProxyConnection::ProxyConnection(const char *origin, x0::HttpRequest *r, bool cloak) :
	x0::HttpMessageProcessor(x0::HttpMessageProcessor::RESPONSE),
	hostname_(nullptr),
	port_(),
	fd_(-1),
	request_(r),
	state_(DISCONNECTED),
	io_(r->connection.worker().loop()),
	timer_(r->connection.worker().loop()),
	connectTimeout_(0),
	readTimeout_(0),
	writeTimeout_(0),
	writeBuffer_(),
	writeOffset_(0),
	writeProgress_(0),
	writeActive_(false),
	readBuffer_(),
	readActive_(false),
	cloak_(cloak)
{
	TRACE("ProxyConnection()");

	request_->setAbortHandler(&ProxyConnection::onAbort, this);

	io_.set<ProxyConnection, &ProxyConnection::io>(this);
	timer_.set<ProxyConnection, &ProxyConnection::timeout>(this);

	x0::BufferRef ref(origin);
	if (ref.begins("unix:")) {
		// UNIX domain socket
		hostname_ = strdup(origin + 5);
		port_ = 0;
	} else {
		// TCP/IP
		auto pos = ref.rfind(':');
		if (pos != ref.npos) {
			hostname_ = strdup(origin);
			hostname_[pos] = '\0';
			port_ = ref.ref(pos + 1).toInt();
		} else {
			hostname_ = strdup(origin);
			port_ = 80; // default to port 80, if not specified
		}
	}
}

void ProxyConnection::onAbort(void *p)
{
	ProxyConnection *self = reinterpret_cast<ProxyConnection *>(p);

	delete self;
}

ProxyConnection::~ProxyConnection()
{
	TRACE("~ProxyConnection()");

	close();

	if (hostname_) {
		free(hostname_);
	}

	if (request_) {
		request_->finish();
	}
}

ConnectResult ProxyConnection::openUnix(const std::string& unixPath, int* out_fd)
{
	TRACE("connect(unix=%s)", unixPath.c_str());

	int fd = ::socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		TRACE("proxy: socket creation error: %s",  strerror(errno));
		return ConnectResult::Failed;
	}

	struct sockaddr_un addr;
	addr.sun_family = AF_UNIX;
	size_t addrlen = sizeof(addr.sun_family)
		+ strlen(strncpy(addr.sun_path, unixPath.c_str(), sizeof(addr.sun_path)));

	if (::connect(fd, (struct sockaddr*) &addr, addrlen) < 0) {
		TRACE("proxy: could not connect to %s: %s", unixPath.c_str(), strerror(errno));
		::close(fd);
		return ConnectResult::Failed;
	}

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);

	*out_fd = fd;
	return ConnectResult::Established;
}

ConnectResult ProxyConnection::openTcp(const std::string& hostname, int port, int* out_fd)
{
	TRACE("connect(hostname=%s, port=%d)", hostname.c_str(), port);

	struct addrinfo hints;
	struct addrinfo *res;

	ConnectResult result = ConnectResult::Failed;
	int fd = -1;

	std::memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char sport[16];
	snprintf(sport, sizeof(sport), "%d", port);

	int rv = getaddrinfo(hostname.c_str(), sport, &hints, &res);
	if (rv) {
		TRACE("proxy: could not get addrinfo of %s:%s: %s", hostname.c_str(), sport, gai_strerror(rv));
		return ConnectResult::Failed;
	}

	for (struct addrinfo *rp = res; rp != nullptr; rp = rp->ai_next) {
		fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0) {
			TRACE("proxy: socket creation error: %s",  strerror(errno));
			continue;
		}

		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);

		rv = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
		if (rv == 0) {
			TRACE("connect: instant success (fd:%d)", fd);
			result = ConnectResult::Established;
			break;
		} else if (/*rv < 0 &&*/ errno == EINPROGRESS) {
			TRACE("connect: backgrounding (fd:%d)", fd);
			result = ConnectResult::InProgress;
			break;
		} else {
			TRACE("proxy: could not connect to %s:%s: %s", hostname.c_str(), sport, strerror(errno));
			::close(fd);
			fd = -1;
		}
	}

	*out_fd = fd;
	freeaddrinfo(res);
	return result;
}

void ProxyConnection::start()
{
	// request line
	writeBuffer_.push_back(request_->method);
	writeBuffer_.push_back(' ');
	writeBuffer_.push_back(request_->uri);
	writeBuffer_.push_back(" HTTP/1.1\r\n");

	// request headers
	for (auto i = request_->requestHeaders.begin(), e = request_->requestHeaders.end(); i != e; ++i)
	{
		if (iequals(i->name, "Content-Transfer")
				|| iequals(i->name, "Expect")
				|| iequals(i->name, "Connection"))
			continue;

		TRACE("pass requestHeader(%s: %s)", i->name.str().c_str(), i->value.str().c_str());
		writeBuffer_.push_back(i->name);
		writeBuffer_.push_back(": ");
		writeBuffer_.push_back(i->value);
		writeBuffer_.push_back("\r\n");
	}

	// request headers terminator
	writeBuffer_.push_back("\r\n");

	startWrite();
}

/** transferres a request body chunk to the origin server.  */
void ProxyConnection::onRequestChunk(x0::BufferRef&& chunk)
{
	TRACE("onRequestChunk(nb:%ld)", chunk.size());
	writeBuffer_.push_back(chunk);
	io_.start();
	startWrite();
}

inline bool validateResponseHeader(const x0::BufferRef& name)
{
	// XXX do not allow origin's connection-level response headers to be passed to the client.
	if (iequals(name, "Connection"))
		return false;

	if (iequals(name, "Transfer-Encoding"))
		return false;

	return true;
}

/** callback, invoked when the origin server has passed us the response status line.
 *
 * We will use the status code only.
 * However, we could pass the text field, too - once x0 core supports it.
 */
void ProxyConnection::messageBegin(int major, int minor, int code, x0::BufferRef&& text)
{
	TRACE("ProxyConnection(%p).status(HTTP/%d.%d, %d, '%s')", (void*)this, major, minor, code, text.str().c_str());

	request_->status = static_cast<x0::HttpError>(code);
	TRACE("status: %d", (int)request_->status);
}

/** callback, invoked on every successfully parsed response header.
 *
 * We will pass this header directly to the client's response,
 * if that is NOT a connection-level header.
 */
void ProxyConnection::messageHeader(x0::BufferRef&& name, x0::BufferRef&& value)
{
	TRACE("ProxyConnection(%p).onHeader('%s', '%s')", (void*)this, name.str().c_str(), value.str().c_str());

	if (!validateResponseHeader(name))
		return;

	if (cloak_ && iequals(name, "Server"))
		return;

	request_->responseHeaders.push_back(name.str(), value.str());
}

/** callback, invoked on a new response content chunk. */
bool ProxyConnection::messageContent(x0::BufferRef&& chunk)
{
	TRACE("messageContent(nb:%lu) state:%s", chunk.size(), state_str());

	// stop watching for more input
	io_.stop();

	// transfer response-body chunk to client
	request_->write<x0::BufferSource>(std::move(chunk));

	// start listening on backend I/O when chunk has been fully transmitted
	request_->writeCallback([&]() {
		TRACE("chunk write complete: %s", state_str());
		io_.start();
	});

	return true;
}

bool ProxyConnection::messageEnd()
{
	TRACE("messageEnd() state:%s", state_str());
	return true;
}

void ProxyConnection::startRead()
{
	TRACE("startRead(%s)", state_str());
	switch (state_)
	{
		case DISCONNECTED:
			// invalid state to start reading from
			break;
		case ABOUT_TO_CONNECT:
			TRACE("invalid state!");
			break;
		case CONNECTING:
			// we're invoked from within onConnectComplete()
			state_ = CONNECTED;
			io_.set(fd_, ev::READ);

			//FIXME needed? onConnected();

			break;
		case CONNECTED:
			// invalid state to start reading from
			break;
		case WRITING:
			if (readTimeout_ > 0)
				timer_.start(readTimeout_, 0.0);

			state_ = READING;
			io_.set(fd_, ev::READ);
			break;
		case READING:
			// continue reading
			if (readTimeout_ > 0)
				timer_.start(readTimeout_, 0.0);

			break;
	}
}

bool ProxyConnection::connect()
{
	TRACE("connect(): hostname=%s, port=%d) %s", hostname_, port_, state_str());

	ConnectResult rv = port_
		? openTcp(hostname_, port_, &fd_)
		: openUnix(hostname_, &fd_);

	switch (rv) {
	case ConnectResult::Established:
		TRACE("connect: instant success");
		state_ = CONNECTED;
		startWrite();
		return true;
	case ConnectResult::InProgress:
		TRACE("connect: backgrounding");
		state_ = ABOUT_TO_CONNECT;
		startWrite();
		return true;
	case ConnectResult::Failed:
	default:
		TRACE("connect: failed");
		return false;
	}
}

void ProxyConnection::close()
{
	if (fd_ < 0)
		return;

	timer_.stop();
	io_.stop();

	::close(fd_);
	fd_ = 0;
}

void ProxyConnection::destroy(x0::HttpError code)
{
	if (fd_ >= 0) {
		close();
	}

	if (request_)
	{
		if (code != x0::HttpError::Undefined)
			request_->status = code;

		request_->finish();
		request_ = nullptr;
	}

	delete this;
}

void ProxyConnection::startWrite()
{
	TRACE("startWrite(%s)", state_str());

	switch (state_)
	{
		case DISCONNECTED:
			if (!connect()) {
				destroy(x0::HttpError::ServiceUnavailable);
			}
			break;
		case ABOUT_TO_CONNECT:
			// initiated asynchronous connect: watch for completeness

			if (connectTimeout_ > 0)
				timer_.start(connectTimeout_, 0.0);

			io_.set(fd_, ev::WRITE);
			io_.start();
			state_ = CONNECTING;
			break;
		case CONNECTING:
			// asynchronous-connect completed and request committed already: start writing

			if (writeTimeout_ > 0)
				timer_.start(writeTimeout_, 0.0);

			state_ = WRITING;
			break;
		case CONNECTED:
			if (writeTimeout_ > 0)
				timer_.start(writeTimeout_, 0.0);

			state_ = WRITING;
			io_.set(fd_, ev::WRITE);
			io_.start();
			break;
		case WRITING:
			// continue writing
			break;
		case READING:
			if (writeTimeout_ > 0)
				timer_.start(writeTimeout_, 0.0);

			state_ = WRITING;
			io_.set(fd_, ev::WRITE);
			break;
	}
}

void ProxyConnection::io(ev::io& w, int revents)
{
	TRACE("io(0x%04x) timer_active:%d", revents, timer_.is_active());

	if (timer_.is_active())
		timer_.stop();

	if (revents & ev::READ)
		readSome();

	if (revents & ev::WRITE)
	{
		if (state_ != CONNECTING)
			writeSome();
		else
			onConnectComplete();
	}
}

void ProxyConnection::timeout(ev::timer&, int revents)
{
	TRACE("timeout"); // TODO
}

/** callback, invoked when asynchronous connect completed.
 */
void ProxyConnection::onConnectComplete()
{
	int val = 0;
	socklen_t vlen = sizeof(val);
	if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &val, &vlen) == 0)
	{
		if (val == 0)
		{
			TRACE("onConnectComplete: connected");
			// start writing the request immediately
			startWrite();
		}
		else
		{
			TRACE("onConnectComplete: error(%d): %s", val, strerror(val));
			destroy(x0::HttpError::ServiceUnavailable);
		}
	}
	else
	{
		TRACE("onConnectComplete: getsocketopt() error: %s", strerror(errno));
		destroy(x0::HttpError::ServiceUnavailable);
	}
}

void ProxyConnection::writeSome()
{
	TRACE("writeSome() - %s (%d)", state_str(), request_->contentAvailable());

	ssize_t rv = ::write(fd_, writeBuffer_.data() + writeOffset_, writeBuffer_.size() - writeOffset_);

	if (rv > 0)
	{
		TRACE("write request: %ld (of %ld) bytes", rv, writeBuffer_.size() - writeOffset_);

		writeOffset_ += rv;
		writeProgress_ += rv;

		if (writeOffset_ == writeBuffer_.size())
		{
			TRACE("writeOffset == writeBuffser.size (%ld) p:%ld, ca: %d, clr:%ld",
				writeOffset_,
				writeProgress_,
				request_->contentAvailable(),
				request_->connection.contentLength());

			writeOffset_ = 0;
			writeBuffer_.clear();

			if (request_->contentAvailable()) {
				io_.stop();
				request_->read(std::bind(&ProxyConnection::onRequestChunk, this, std::placeholders::_1));
			} else {
				// request fully transmitted, let's read response then.
				startRead();
			}
		}
	}
	else
	{
		TRACE("write request failed(%ld): %s", rv, strerror(errno));
		close();
	}
}

void ProxyConnection::readSome()
{
	TRACE("readSome() - %s", state_str());

	std::size_t lower_bound = readBuffer_.size();

	if (lower_bound == readBuffer_.capacity())
		readBuffer_.setCapacity(lower_bound + 4096);

	ssize_t rv = ::read(fd_, (char *)readBuffer_.end(), readBuffer_.capacity() - lower_bound);

	if (rv > 0)
	{
		TRACE("read response: %ld bytes", rv);
		readBuffer_.resize(lower_bound + rv);

		std::size_t np = 0;
		std::error_code ec = process(readBuffer_.ref(lower_bound, rv), np);
		TRACE("readSome(): process(): %s", ec.message().c_str());
		if (ec == x0::HttpMessageError::Success) {
			destroy();
		} else if (ec == x0::HttpMessageError::Partial) {
			TRACE("resume with io_active:%d, state:%s", io_.is_active(), state_str());
			startRead();
		} else {
			destroy(x0::HttpError::InternalServerError);
		}
	}
	else if (rv == 0)
	{
		TRACE("http server (%d) connection closed", fd_);
		close();
	}
	else
	{
		TRACE("read response failed(%ld): %s", rv, strerror(errno));

		if (errno == EAGAIN)
			startRead();
		else
			close();
	}
}

// }}}

// {{{ plugin class
/**
 * \ingroup plugins
 * \brief proxy content generator plugin
 */
class proxy_plugin :
	public x0::HttpPlugin
{
private:
	bool cloak_;

public:
	proxy_plugin(x0::HttpServer& srv, const std::string& name) :
		x0::HttpPlugin(srv, name),
		cloak_(true)
	{
		registerHandler<proxy_plugin, &proxy_plugin::proxy_reverse>("proxy.reverse");
		registerSetupProperty<proxy_plugin, &proxy_plugin::proxy_cloak>("proxy.cloak", x0::FlowValue::BOOLEAN);
	}

	~proxy_plugin()
	{
	}

private:
	void proxy_cloak(x0::FlowValue& result, const x0::Params& args)
	{
		if (args.count() && (args[0].isBool() || args[0].isNumber()))
		{
			cloak_ = args[0].toBool();
			printf("proxy cloak: %s\n", cloak_ ? "true" : "false");
		}

		result.set(cloak_);
	}

	bool proxy_reverse(x0::HttpRequest *r, const x0::Params& args)
	{
		// TODO: reuse already spawned proxy connections instead of recreating each time.

		const char *origin = args[0].toString();
		ProxyConnection *pc = new ProxyConnection(origin, r, cloak_);
		if (!pc)
			return false; // XXX handle as 500 instead?

		pc->start();

		return true;
	}
};
// }}}

X0_EXPORT_PLUGIN(proxy)
