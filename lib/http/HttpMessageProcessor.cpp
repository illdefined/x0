/* <x0/HttpMessageProcessor.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.io/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#include <x0/http/HttpMessageProcessor.h>

namespace x0 {

#if 0
#	define TRACE(msg...) DEBUG("HttpMessageProcessor: " msg)
#else
#	define TRACE(msg...)
#endif

//! Support messages using LF-only as linefeed instead of CRLF,
//! which is not HTTP conform.
#define X0_HTTP_SUPPORT_SHORT_LF 1

// {{{ const std::error_category& http_message_category() throw()
class http_message_category_impl :
	public std::error_category
{
private:
	std::string codes_[32];

	void set(HttpMessageError ec, const std::string& message)
	{
		codes_[static_cast<int>(ec)] = message;
	}

	void initialize_codes()
	{
		for (std::size_t i = 0; i < sizeof(codes_) / sizeof(*codes_); ++i)
			codes_[i] = "Undefined";

		set(HttpMessageError::Success, "Success");
		set(HttpMessageError::Partial, "Partial");
		set(HttpMessageError::Aborted, "Aborted");
		set(HttpMessageError::SyntaxError, "Invalid Syntax");
	}

public:
	http_message_category_impl()
	{
		initialize_codes();
	}

	virtual const char *name() const
	{
		return "HttpMessage";
	}

	virtual std::string message(int ec) const
	{
		return codes_[ec];
	}
};

http_message_category_impl http_message_category_impl_;

const std::error_category& http_message_category() throw()
{
	return http_message_category_impl_;
}
// }}}

/** initializes the HTTP/1.1 message processor.
 *
 * \param mode REQUEST: parses and processes an HTTP/1.1 Request,
 *             RESPONSE: parses and processes an HTTP/1.1 Response.
 *             MESSAGE: parses and processes an HTTP/1.1 message, that is, without the first request/status line - just headers and content.
 *
 * \note No member variable may be modified after the hook invokation returned with
 *       a false return code, which means, that processing is to be cancelled
 *       and thus, may imply, that the object itself may have been already deleted.
 */
HttpMessageProcessor::HttpMessageProcessor(ParseMode mode) :
	mode_(mode),
	state_(MESSAGE_BEGIN),
	lwsNext_(),
	lwsNull_(),
	method_(),
	entity_(),
	versionMajor_(),
	versionMinor_(),
	code_(0),
	message_(),
	name_(),
	value_(),
	chunked_(false),
	contentLength_(-1),
	filters_()
{
}

inline bool HttpMessageProcessor::isChar(char value)
{
	return static_cast<unsigned>(value) >= 0 && static_cast<unsigned>(value) <= 127;
}

inline bool HttpMessageProcessor::isControl(char value)
{
	return (value >= 0 && value <= 31) || value == 127;
}

inline bool HttpMessageProcessor::isSeparator(char value)
{
	switch (value)
	{
		case '(':
		case ')':
		case '<':
		case '>':
		case '@':
		case ',':
		case ';':
		case ':':
		case '\\':
		case '"':
		case '/':
		case '[':
		case ']':
		case '?':
		case '=':
		case '{':
		case '}':
		case SP:
		case HT:
			return true;
		default:
			return false;
	}
}

inline bool HttpMessageProcessor::isToken(char value)
{
	return isChar(value) && !(isControl(value) || isSeparator(value));
}

inline bool HttpMessageProcessor::isText(char value)
{
	// TEXT = <any OCTET except CTLs but including LWS>
	return !isControl(value) || value == SP || value == HT;
}

// {{{ HttpMessageProcessor hook stubs
/** hook, invoked for each HTTP/1.1 Request-Line, that has been fully parsed.
 *
 * \param method the request-method (e.g. GET or POST)
 * \param uri the requested URI (e.g. /index.html)
 * \param versionMajor HTTP major version (e.g. 0 for 0.9)
 * \param versionMinor HTTP minor version (e.g. 9 for 0.9)
 */
bool HttpMessageProcessor::onMessageBegin(const BufferRef& method, const BufferRef& uri, int versionMajor, int versionMinor)
{
	return true;
}

/** hook, invoked for each HTTP/1.1 Status-Line, that has been fully parsed.
 *
 * \param versionMajor HTTP major version (e.g. 0 for 0.9)
 * \param versionMinor HTTP minor version (e.g. 9 for 0.9)
 * \param code HTTP response status code (e.g. 200 or 404)
 * \param text HTTP response status text (e.g. "Ok" or "Not Found")
 */
bool HttpMessageProcessor::onMessageBegin(int versionMajor, int versionMinor, int code, const BufferRef& text)
{
	return true;
}

/** hook, invoked for each generic HTTP Message.
 */
bool HttpMessageProcessor::onMessageBegin()
{
	return true;
}

/** hook, invoked for each sequentially parsed HTTP header.
 */
bool HttpMessageProcessor::onMessageHeader(const BufferRef& name, const BufferRef& value)
{
	return true;
}

/** hook, invoked once all request headers have been fully parsed (no possible content parsed yet).
 *
 * \note The default behavior (if not overridden) is, to do nothing and return true.
 *
 * \retval true continue processing further content (if any)
 * \retval false abort message processing
 */
bool HttpMessageProcessor::onMessageHeaderEnd()
{
	return true;
}

/** hook, invoked for every chunk of message content being processed.
 *
 * \note The default behavior (if not overridden) is, to do nothing and return true.
 *
 * \retval true continue processing further content (if any)
 * \retval false abort message processing
 */
bool HttpMessageProcessor::onMessageContent(const BufferRef& chunk)
{
	return true;
}

/** hook, invoked once a fully HTTP message has been processed.
 *
 * \note The default behavior (if not overridden) is, to do nothing and return true.
 *
 * \retval true continue processing further content (if any)
 * \retval false abort message processing
 */
bool HttpMessageProcessor::onMessageEnd()
{
	return true;
}
// }}}

const char *HttpMessageProcessor::state_str() const
{
	switch (state_) {
		// artificial
		case HttpMessageProcessor::SYNTAX_ERROR: return "syntax-error";
		case HttpMessageProcessor::MESSAGE_BEGIN: return "message-begin";

		// request-line
		case HttpMessageProcessor::REQUEST_LINE_BEGIN: return "request-line-begin";
		case HttpMessageProcessor::REQUEST_METHOD: return "request-method";
		case HttpMessageProcessor::REQUEST_ENTITY_BEGIN: return "request-entity-begin";
		case HttpMessageProcessor::REQUEST_ENTITY: return "request-entity";
		case HttpMessageProcessor::REQUEST_PROTOCOL_BEGIN: return "request-protocol-begin";
		case HttpMessageProcessor::REQUEST_PROTOCOL_T1: return "request-protocol-t1";
		case HttpMessageProcessor::REQUEST_PROTOCOL_T2: return "request-protocol-t2";
		case HttpMessageProcessor::REQUEST_PROTOCOL_P: return "request-protocol-p";
		case HttpMessageProcessor::REQUEST_PROTOCOL_SLASH: return "request-protocol-slash";
		case HttpMessageProcessor::REQUEST_PROTOCOL_VERSION_MAJOR: return "request-protocol-version-major";
		case HttpMessageProcessor::REQUEST_PROTOCOL_VERSION_MINOR: return "request-protocol-version-minor";
		case HttpMessageProcessor::REQUEST_LINE_LF: return "request-line-lf";

		// Status-Line
		case HttpMessageProcessor::STATUS_LINE_BEGIN: return "status-line-begin";
		case HttpMessageProcessor::STATUS_PROTOCOL_BEGIN: return "status-protocol-begin";
		case HttpMessageProcessor::STATUS_PROTOCOL_T1: return "status-protocol-t1";
		case HttpMessageProcessor::STATUS_PROTOCOL_T2: return "status-protocol-t2";
		case HttpMessageProcessor::STATUS_PROTOCOL_P: return "status-protocol-t2";
		case HttpMessageProcessor::STATUS_PROTOCOL_SLASH: return "status-protocol-t2";
		case HttpMessageProcessor::STATUS_PROTOCOL_VERSION_MAJOR: return "status-protocol-version-major";
		case HttpMessageProcessor::STATUS_PROTOCOL_VERSION_MINOR: return "status-protocol-version-minor";
		case HttpMessageProcessor::STATUS_CODE_BEGIN: return "status-code-begin";
		case HttpMessageProcessor::STATUS_CODE: return "status-code";
		case HttpMessageProcessor::STATUS_MESSAGE_BEGIN: return "status-message-begin";
		case HttpMessageProcessor::STATUS_MESSAGE: return "status-message";
		case HttpMessageProcessor::STATUS_MESSAGE_LF: return "status-message-lf";

		// message header
		case HttpMessageProcessor::HEADER_NAME_BEGIN: return "header-name-begin";
		case HttpMessageProcessor::HEADER_NAME: return "header-name";
		case HttpMessageProcessor::HEADER_COLON: return "header-colon";
		case HttpMessageProcessor::HEADER_VALUE_BEGIN: return "header-value-begin";
		case HttpMessageProcessor::HEADER_VALUE: return "header-value";
		case HttpMessageProcessor::HEADER_VALUE_LF: return "header-value-lf";
		case HttpMessageProcessor::HEADER_VALUE_END: return "header-value-end";
		case HttpMessageProcessor::HEADER_END_LF: return "header-end-lf";

		// LWS
		case HttpMessageProcessor::LWS_BEGIN: return "lws-begin";
		case HttpMessageProcessor::LWS_LF: return "lws-lf";
		case HttpMessageProcessor::LWS_SP_HT_BEGIN: return "lws-sp-ht-begin";
		case HttpMessageProcessor::LWS_SP_HT: return "lws-sp-ht";

		// message content
		case HttpMessageProcessor::CONTENT_BEGIN: return "content-begin";
		case HttpMessageProcessor::CONTENT: return "content";
		case HttpMessageProcessor::CONTENT_ENDLESS: return "content-endless";
		case HttpMessageProcessor::CONTENT_CHUNK_SIZE_BEGIN: return "content-chunk-size-begin";
		case HttpMessageProcessor::CONTENT_CHUNK_SIZE: return "content-chunk-size";
		case HttpMessageProcessor::CONTENT_CHUNK_LF1: return "content-chunk-lf1";
		case HttpMessageProcessor::CONTENT_CHUNK_BODY: return "content-chunk-body";
		case HttpMessageProcessor::CONTENT_CHUNK_LF2: return "content-chunk-lf2";
		case HttpMessageProcessor::CONTENT_CHUNK_CR3: return "content-chunk-cr3";
		case HttpMessageProcessor::CONTENT_CHUNK_LF3: return "content-chunk_lf3";
	}
	return "UNKNOWN";
}

/** processes a message-chunk.
 *
 * \param chunk   the chunk of bytes to process
 *
 * \return        number of bytes actually parsed and processed
 */
std::size_t HttpMessageProcessor::process(const BufferRef& chunk, size_t* out_nparsed)
{
	/*
	 * CR               = 0x0D
	 * LF               = 0x0A
	 * SP               = 0x20
	 * HT               = 0x09
	 *
	 * CRLF             = CR LF
	 * LWS              = [CRLF] 1*( SP | HT )
	 *
	 * HTTP-message     = Request | Response
	 *
	 * generic-message  = start-line
	 *                    *(message-header CRLF)
	 *                    CRLF
	 *                    [ message-body ]
	 *                 
	 * start-line       = Request-Line | Status-Line
	 *
	 * Request-Line     = Method SP Request-URI SP HTTP-Version CRLF
	 *
	 * Method           = "OPTIONS" | "GET" | "HEAD"
	 *                  | "POST"    | "PUT" | "DELETE"
	 *                  | "TRACE"   | "CONNECT"
	 *                  | extension-method
	 *
	 * Request-URI      = "*" | absoluteURI | abs_path | authority
	 *
	 * extension-method = token
	 *
	 * Status-Line      = HTTP-Version SP Status-Code SP Reason-Phrase CRLF
	 *
	 * HTTP-Version     = "HTTP" "/" 1*DIGIT "." 1*DIGIT
	 * Status-Code      = 3*DIGIT
	 * Reason-Phrase    = *<TEXT, excluding CR, LF>
	 *
	 * absoluteURI      = "http://" [user ':' pass '@'] hostname [abs_path] [qury]
	 * abs_path         = "/" *CHAR
	 * authority        = ...
	 * token            = 1*<any CHAR except CTLs or seperators>
	 * separator        = "(" | ")" | "<" | ">" | "@"
	 *                  | "," | ";" | ":" | "\" | <">
	 *                  | "/" | "[" | "]" | "?" | "="
	 *                  | "{" | "}" | SP | HT
	 *
	 * message-header   = field-name ":" [ field-value ]
	 * field-name       = token
	 * field-value      = *( field-content | LWS )
	 * field-content    = <the OCTETs making up the field-value
	 *                    and consisting of either *TEXT or combinations
	 *                    of token, separators, and quoted-string>
	 *
	 * message-body     = entity-body
	 *                  | <entity-body encoded as per Transfer-Encoding>
	 */

	const char *i = chunk.begin();
	const char *e = chunk.end();

	size_t result = chunk.offset();
	size_t* nparsed = out_nparsed ? out_nparsed : &result;

	//TRACE("process(curState:%s): size: %ld: '%s'", state_str(), chunk.size(), chunk.str().c_str());
	TRACE("process(curState:%s): size: %ld", state_str(), chunk.size());

#if 0
	switch (state_) {
		case CONTENT: // fixed size content
			if (!passContent(chunk, nparsed))
				goto done;

			i += *nparsed;
			break;
		case CONTENT_ENDLESS: // endless-sized content (until stream end)
		{
			*nparsed += chunk.size();
			bool rv = filters_.empty()
				? onMessageContent(chunk)
				: onMessageContent(filters_.process(chunk));

			goto done;
		}
		default:
			break;
	}
#endif

	while (i != e)
	{
#if 0
		if (std::isprint(*i)) {
			TRACE("parse: %4ld, 0x%02X (%c),  %s", *nparsed, *i, *i, state_str());
		} else {
			TRACE("parse: %4ld, 0x%02X,     %s", *nparsed, *i, state_str());
		}
#endif

		switch (state_)
		{
			case MESSAGE_BEGIN:
				contentLength_ = -1;
				switch (mode_) {
					case REQUEST:
						state_ = REQUEST_LINE_BEGIN;
						versionMajor_ = 0;
						versionMinor_ = 0;
						break;
					case RESPONSE:
						state_ = STATUS_LINE_BEGIN;
						code_ = 0;
						break;
					case MESSAGE:
						state_ = HEADER_NAME_BEGIN;

						// an internet message has no special top-line,
						// so we just invoke the callback right away
						if (!onMessageBegin())
							goto done;

						break;
				}
				break;
			case REQUEST_LINE_BEGIN:
				if (isToken(*i)) {
					state_ = REQUEST_METHOD;
					method_ = chunk.ref(*nparsed - chunk.offset(), 1);

					++*nparsed;
					++i;
				} else {
					state_ = SYNTAX_ERROR;
				}
				break;
			case REQUEST_METHOD:
				if (*i == SP) {
					state_ = REQUEST_ENTITY_BEGIN;
					++*nparsed;
					++i;
				} else if (!isToken(*i)) {
					state_ = SYNTAX_ERROR;
				} else {
					method_.shr();
					++*nparsed;
					++i;
				}
				break;
			case REQUEST_ENTITY_BEGIN:
				if (std::isprint(*i)) {
					entity_ = chunk.ref(*nparsed - chunk.offset(), 1);
					state_ = REQUEST_ENTITY;

					++*nparsed;
					++i;
				}
				else {
					state_ = SYNTAX_ERROR;
				}
				break;
			case REQUEST_ENTITY:
				if (*i == SP) {
					state_ = REQUEST_PROTOCOL_BEGIN;
					++*nparsed;
					++i;
				} else if (std::isprint(*i)) {
					entity_.shr();
					++*nparsed;
					++i;
				} else
					state_ = SYNTAX_ERROR;
				break;
			case REQUEST_PROTOCOL_BEGIN:
				if (*i != 'H') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = REQUEST_PROTOCOL_T1;
					++*nparsed;
					++i;
				}
				break;
			case REQUEST_PROTOCOL_T1:
				if (*i != 'T') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = REQUEST_PROTOCOL_T2;
					++*nparsed;
					++i;
				}
				break;
			case REQUEST_PROTOCOL_T2:
				if (*i != 'T') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = REQUEST_PROTOCOL_P;
					++*nparsed;
					++i;
				}
				break;
			case REQUEST_PROTOCOL_P:
				if (*i != 'P') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = REQUEST_PROTOCOL_SLASH;
					++*nparsed;
					++i;
				}
				break;
			case REQUEST_PROTOCOL_SLASH:
				if (*i != '/') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = REQUEST_PROTOCOL_VERSION_MAJOR;
					++*nparsed;
					++i;
				}
				break;
			case REQUEST_PROTOCOL_VERSION_MAJOR:
				if (*i == '.') {
					state_ = REQUEST_PROTOCOL_VERSION_MINOR;
					++*nparsed;
					++i;
				}
				else if (!std::isdigit(*i)) {
					state_ = SYNTAX_ERROR;
				} else {
					versionMajor_ = versionMajor_ * 10 + *i - '0';
					++*nparsed;
					++i;
				}
				break;
			case REQUEST_PROTOCOL_VERSION_MINOR:
				if (*i == CR) {
					state_ = REQUEST_LINE_LF;
					++*nparsed;
					++i;
				}
#if defined(X0_HTTP_SUPPORT_SHORT_LF)
				else if (*i == LF) {
					state_ = HEADER_NAME_BEGIN;
					++*nparsed;
					++i;

					TRACE("request-line: method=%s, entity=%s, vmaj=%d, vmin=%d",
							method_.str().c_str(), entity_.str().c_str(), versionMajor_, versionMinor_);

					if (!onMessageBegin(method_, entity_, versionMajor_, versionMinor_)) {
						goto done;
					}
				}
#endif
				else if (!std::isdigit(*i)) {
					state_ = SYNTAX_ERROR;
				} else {
					versionMinor_ = versionMinor_ * 10 + *i - '0';
					++*nparsed;
					++i;
				}
				break;
			case REQUEST_LINE_LF:
				if (*i == LF) {
					state_ = HEADER_NAME_BEGIN;
					++*nparsed;
					++i;

					TRACE("request-line: method=%s, entity=%s, vmaj=%d, vmin=%d",
							method_.str().c_str(), entity_.str().c_str(), versionMajor_, versionMinor_);

					if (!onMessageBegin(method_, entity_, versionMajor_, versionMinor_)) {
						goto done;
					}
				}
				else
					state_ = SYNTAX_ERROR;
				break;
			case STATUS_LINE_BEGIN:
			case STATUS_PROTOCOL_BEGIN:
				if (*i != 'H') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = STATUS_PROTOCOL_T1;
					++*nparsed;
					++i;
				}
				break;
			case STATUS_PROTOCOL_T1:
				if (*i != 'T') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = STATUS_PROTOCOL_T2;
					++*nparsed;
					++i;
				}
				break;
			case STATUS_PROTOCOL_T2:
				if (*i != 'T') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = STATUS_PROTOCOL_P;
					++*nparsed;
					++i;
				}
				break;
			case STATUS_PROTOCOL_P:
				if (*i != 'P') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = STATUS_PROTOCOL_SLASH;
					++*nparsed;
					++i;
				}
				break;
			case STATUS_PROTOCOL_SLASH:
				if (*i != '/') {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = STATUS_PROTOCOL_VERSION_MAJOR;
					++*nparsed;
					++i;
				}
				break;
			case STATUS_PROTOCOL_VERSION_MAJOR:
				if (*i == '.') {
					state_ = STATUS_PROTOCOL_VERSION_MINOR;
					++*nparsed;
					++i;
				} else if (!std::isdigit(*i)) {
					state_ = SYNTAX_ERROR;
				} else {
					versionMajor_ = versionMajor_ * 10 + *i - '0';
					++*nparsed;
					++i;
				}
				break;
			case STATUS_PROTOCOL_VERSION_MINOR:
				if (*i == SP) {
					state_ = STATUS_CODE_BEGIN;
					++*nparsed;
					++i;
				} else if (!std::isdigit(*i)) {
					state_ = SYNTAX_ERROR;
				} else {
					versionMinor_ = versionMinor_ * 10 + *i - '0';
					++*nparsed;
					++i;
				}
				break;
			case STATUS_CODE_BEGIN:
				if (!std::isdigit(*i)) {
					code_ = SYNTAX_ERROR;
					break;
				}
				state_ = STATUS_CODE;
				/* fall through */
			case STATUS_CODE:
				if (std::isdigit(*i)) {
					code_ = code_ * 10 + *i - '0';
					++*nparsed;
					++i;
				} else if (*i == SP) {
					state_ = STATUS_MESSAGE_BEGIN;
					++*nparsed;
					++i;
				} else if (*i == CR) { // no Status-Message passed
					state_ = STATUS_MESSAGE_LF;
					++*nparsed;
					++i;
				} else {
					state_ = SYNTAX_ERROR;
				}
				break;
			case STATUS_MESSAGE_BEGIN:
				if (isText(*i)) {
					state_ = STATUS_MESSAGE;
					message_ = chunk.ref(*nparsed - chunk.offset(), 1);
					++*nparsed;
					++i;
				}
				else
					state_ = SYNTAX_ERROR;
				break;
			case STATUS_MESSAGE:
				if (isText(*i) && *i != CR && *i != LF) {
					message_.shr();
					++*nparsed;
					++i;
				} else if (*i == CR) {
					state_ = STATUS_MESSAGE_LF;
					++*nparsed;
					++i;
				} else {
					state_ = SYNTAX_ERROR;
				}
				break;
			case STATUS_MESSAGE_LF:
				if (*i == LF) {
					state_ = HEADER_NAME_BEGIN;
					++*nparsed;
					++i;

					//TRACE("status-line: HTTP/%d.%d, code=%d, message=%s", versionMajor_, versionMinor_, code_, message_.str().c_str());
					if (!onMessageBegin(versionMajor_, versionMinor_, code_, message_)) {
						goto done;
					}
				} else
					state_ = SYNTAX_ERROR;
				break;
			case HEADER_NAME_BEGIN:
				if (isToken(*i)) {
					name_ = chunk.ref(*nparsed - chunk.offset(), 1);
					state_ = HEADER_NAME;
					++*nparsed;
					++i;
				} else if (*i == CR) {
					state_ = HEADER_END_LF;
					++*nparsed;
					++i;
				}
#if defined(X0_HTTP_SUPPORT_SHORT_LF)
				else if (*i == LF)
					state_ = HEADER_END_LF;
#endif
				else
					state_ = SYNTAX_ERROR;
				break;
			case HEADER_NAME:
				if (isToken(*i)) {
					name_.shr();
					++*nparsed;
					++i;
				} else if (*i == ':') {
					state_ = LWS_BEGIN;
					lwsNext_ = HEADER_VALUE_BEGIN;
					lwsNull_ = HEADER_VALUE_END; // only (CR LF) parsed, assume empty value & go on with next header
					++*nparsed;
					++i;
				} else if (*i == CR) {
					state_ = LWS_LF;
					lwsNext_ = HEADER_COLON;
					lwsNull_ = SYNTAX_ERROR;
					++*nparsed;
					++i;
				} else
					state_ = SYNTAX_ERROR;
				break;
			case HEADER_COLON:
				if (*i == ':') {
					state_ = LWS_BEGIN;
					lwsNext_ = HEADER_VALUE_BEGIN;
					lwsNull_ = HEADER_VALUE_END;
					++*nparsed;
					++i;
				} else
					state_ = SYNTAX_ERROR;
				break;
			case LWS_BEGIN:
				if (*i == CR) {
					state_ = LWS_LF;
					++*nparsed;
					++i;
#if defined(X0_HTTP_SUPPORT_SHORT_LF)
				} else if (*i == LF) {
					state_ = LWS_SP_HT_BEGIN;
					++*nparsed;
					++i;
#endif
				} else if (*i == SP || *i == HT) {
					state_ = LWS_SP_HT;
					++*nparsed;
					++i;
				} else if (std::isprint(*i)) {
					state_ = lwsNext_;
				} else
					state_ = SYNTAX_ERROR;
				break;
			case LWS_LF:
				if (*i == LF) {
					state_ = LWS_SP_HT_BEGIN;
					++*nparsed;
					++i;
				} else
					state_ = SYNTAX_ERROR;
				break;
			case LWS_SP_HT_BEGIN:
				if (*i == SP || *i == HT) {
					if (!value_.empty())
						value_.shr(3); // CR LF (SP | HT)

					state_ = LWS_SP_HT;
					++*nparsed;
					++i;
				} else {
					// only (CF LF) parsed so far and no 1*(SP | HT) found.
					state_ = lwsNull_;
					// XXX no nparsed/i-update
				}
				break;
			case LWS_SP_HT:
				if (*i == SP || *i == HT) {
					if (!value_.empty())
						value_.shr();

					++*nparsed;
					++i;
				} else
					state_ = lwsNext_;
				break;
			case HEADER_VALUE_BEGIN:
				if (isText(*i)) {
					value_ = chunk.ref(*nparsed - chunk.offset(), 1);
					++*nparsed;
					++i;
					state_ = HEADER_VALUE;
				} else if (*i == CR) {
					state_ = HEADER_VALUE_LF;
					++*nparsed;
					++i;
#if defined(X0_HTTP_SUPPORT_SHORT_LF)
				} else if (*i == LF) {
					state_ = HEADER_VALUE_END;
					++*nparsed;
					++i;
#endif
				} else {
					state_ = SYNTAX_ERROR;
				}
				break;
			case HEADER_VALUE:
				if (*i == CR) {
					state_ = LWS_LF;
					lwsNext_ = HEADER_VALUE;
					lwsNull_ = HEADER_VALUE_END;
					++*nparsed;
					++i;
				}
#if defined(X0_HTTP_SUPPORT_SHORT_LF)
				else if (*i == LF) {
					state_ = LWS_SP_HT_BEGIN;
					lwsNext_ = HEADER_VALUE;
					lwsNull_ = HEADER_VALUE_END;
					++*nparsed;
					++i;
				}
#endif
				else if (isText(*i)) {
					value_.shr();
					++*nparsed;
					++i;
				} else
					state_ = SYNTAX_ERROR;
				break;
			case HEADER_VALUE_LF:
				if (*i == LF) {
					state_ = HEADER_VALUE_END;
					++*nparsed;
					++i;
				} else {
					state_ = SYNTAX_ERROR;
				}
				break;
			case HEADER_VALUE_END:
			{
				TRACE("header: name='%s', value='%s'", name_.str().c_str(), value_.str().c_str());

				if (iequals(name_, "Content-Length")) {
					contentLength_ = value_.as<int>();
					TRACE("set content length to: %ld", contentLength_);
				} else if (iequals(name_, "Transfer-Encoding")) {
					if (iequals(value_, "chunked")) {
						chunked_ = true;
					}
				}

				bool rv = onMessageHeader(name_, value_);
				name_.clear();
				value_.clear();

				// continue with the next header
				state_ = HEADER_NAME_BEGIN;

				if (!rv) {
					goto done;
				}
				break;
			}
			case HEADER_END_LF:
				if (*i == LF) {
					bool contentExpected = 
						contentLength_ > 0 
						|| chunked_
						|| mode_ == MESSAGE;

					if (contentExpected)
						state_ = CONTENT_BEGIN;
					else
						state_ = MESSAGE_BEGIN;

					++*nparsed;
					++i;

					if (!onMessageHeaderEnd()) {
						TRACE("messageHeaderEnd returned false. returning `Aborted`-state");
						goto done;
					}

					if (!contentExpected && !onMessageEnd()) {
						goto done;
					}
				} else {
					state_ = SYNTAX_ERROR;
				}
				break;
			case CONTENT_BEGIN:
				if (chunked_)
					state_ = CONTENT_CHUNK_SIZE_BEGIN;
				else if (contentLength_ >= 0)
					state_ = CONTENT;
				else
					state_ = CONTENT_ENDLESS;
				break;
			case CONTENT_ENDLESS: // body w/o content-length (allowed in simple MESSAGE types only)
			{
				BufferRef c(chunk.ref(*nparsed - chunk.offset()));

				*nparsed += c.size();
				i += c.size();

				bool rv = filters_.empty()
					? onMessageContent(c)
					: onMessageContent(filters_.process(c));

				if (!rv)
					goto done;

				break;
			}
			case CONTENT: // fixed size content length
			{
				std::size_t chunkSize = std::min(static_cast<size_t>(contentLength_), chunk.size() - (*nparsed - chunk.offset()));

				std::size_t offset = *nparsed - chunk.offset();

				contentLength_ -= chunkSize;
				*nparsed += chunkSize;
				i += chunkSize;

				bool rv = filters_.empty()
					? onMessageContent(chunk.ref(offset, chunkSize))
					: onMessageContent(filters_.process(chunk.ref(offset, chunkSize)));

				if (contentLength_ == 0)
					state_ = MESSAGE_BEGIN;

				if (!rv)
					goto done;

				if (state_ == MESSAGE_BEGIN && !onMessageEnd())
					goto done;

				break;
			}
			case CONTENT_CHUNK_SIZE_BEGIN:
				if (!std::isxdigit(*i)) {
					state_ = SYNTAX_ERROR;
					break;
				}
				state_ = CONTENT_CHUNK_SIZE;
				contentLength_ = 0;
				/* fall through */
			case CONTENT_CHUNK_SIZE:
				if (*i == CR) {
					state_ = CONTENT_CHUNK_LF1;
					++*nparsed;
					++i;
				} else if (*i >= '0' && *i <= '9') {
					contentLength_ = contentLength_ * 16 + *i - '0';
					++*nparsed;
					++i;
				} else if (*i >= 'a' && *i <= 'f') {
					contentLength_ = contentLength_ * 16 + 10 + *i - 'a';
					++*nparsed;
					++i;
				} else if (*i >= 'A' && *i <= 'F') {
					contentLength_ = contentLength_ * 16 + 10 + *i - 'A';
					++*nparsed;
					++i;
				} else {
					state_ = SYNTAX_ERROR;
				}
				break;
			case CONTENT_CHUNK_LF1:
				if (*i != LF) {
					state_ = SYNTAX_ERROR;
				} else {
					//TRACE("content_length: %ld", contentLength_);
					if (contentLength_ != 0)
						state_ = CONTENT_CHUNK_BODY;
					else
						state_ = CONTENT_CHUNK_CR3;

					++*nparsed;
					++i;
				}
				break;
			case CONTENT_CHUNK_BODY:
				if (contentLength_) {
					std::size_t chunkSize = std::min(static_cast<size_t>(contentLength_), chunk.size() - (*nparsed - chunk.offset()));

					std::size_t offset = *nparsed - chunk.offset();
					contentLength_ -= chunkSize;
					*nparsed += chunkSize;
					i += chunkSize;

					bool rv = filters_.empty()
						? onMessageContent(chunk.ref(offset, chunkSize))
						: onMessageContent(filters_.process(chunk.ref(offset, chunkSize)));

					if (!rv) {
						goto done;
					}
				} else if (*i == CR) {
					state_ = CONTENT_CHUNK_LF2;
					++*nparsed;
					++i;
				}
				break;
			case CONTENT_CHUNK_LF2:
				if (*i != LF) {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = CONTENT_CHUNK_SIZE;
					++*nparsed;
					++i;
				}
				break;
			case CONTENT_CHUNK_CR3:
				if (*i != CR) {
					state_ = SYNTAX_ERROR;
				} else {
					state_ = CONTENT_CHUNK_LF3;
					++*nparsed;
					++i;
				}
				break;
			case CONTENT_CHUNK_LF3:
				if (*i != LF) {
					state_ = SYNTAX_ERROR;
				} else {
					++*nparsed;
					++i;

					if (!onMessageEnd())
						goto done;

					state_ = MESSAGE_BEGIN;
				}
				break;
			case SYNTAX_ERROR:
			{
#if !defined(NDEBUG)
				TRACE("parse: syntax error");
				if (std::isprint(*i)) {
					TRACE("parse: syntax error at nparsed: %ld, character: '%c'", *nparsed, *i);
				} else {
					TRACE("parse: syntax error at nparsed: %ld, character: 0x%02X", *nparsed, *i);
				}
				Buffer::dump(chunk.data(), chunk.size(), "request chunk (at syntax error)");
#endif
				goto done;
			}
			default:
#if !defined(NDEBUG)
				TRACE("parse: unknown state %i", state_);
				if (std::isprint(*i)) {
					TRACE("parse: internal error at nparsed: %ld, character: '%c'", *nparsed, *i);
				} else {
					TRACE("parse: internal error at nparsed: %ld, character: 0x%02X", *nparsed, *i);
				}
				Buffer::dump(chunk.data(), chunk.size(), "request chunk (at unknown state)");
#endif
				goto done;
		}
	}
	// we've reached the end of the chunk

	if (state_ == CONTENT_BEGIN) {
		// we've just parsed all headers but no body yet.

		if (contentLength_ < 0 && !chunked_ && mode_ != MESSAGE) {
			// and there's no body to come

			if (!onMessageEnd())
				goto done;

			// subsequent calls to process() parse next request(s).
			state_ = MESSAGE_BEGIN;
		}
	}

done:
	return *nparsed - chunk.offset();
}

} // namespace x0
