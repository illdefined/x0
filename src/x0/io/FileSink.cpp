#include <x0/io/FileSink.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace x0 {

FileSink::FileSink(const std::string& filename) :
	SystemSink(open(filename.c_str(), O_WRONLY | O_CREAT, 0666))
{
	fcntl(handle_, F_SETFL, O_CLOEXEC, 1);
}

FileSink::~FileSink()
{
	::close(handle_);
}

} // namespace x0