#include <solib-handler.hh>
#include <collector.hh>
#include <output-handler.hh>
#include <configuration.hh>
#include <capabilities.hh>
#include <file-parser.hh>
#include <utils.hh>
#include <phdr_data.h>
#include <generated-data-base.hh>

#include <mutex>
#include <vector>
#include <unordered_map>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

using namespace kcov;

extern GeneratedData __library_data;

class SolibHandler : public ISolibHandler, ICollector::IEventTickListener
{
public:
	SolibHandler(IFileParser &parser, ICollector &collector) :
		m_ldPreloadString(NULL),
		m_envString(NULL),
		m_solibFd(-1),
		m_solibThreadValid(false),
		m_threadShouldExit(false),
		m_parser(&parser)
{
		memset(&m_solibThread, 0, sizeof(m_solibThread));

		// Only useful for ELF binaries
		if (parser.getParserType() == "ELF" && !IConfiguration::getInstance().keyAsInt("gcov"))
			collector.registerEventTickListener(*this);

		// Skip this very special library
		m_foundSolibs["libkcov_sowrapper.so"] = true;
	}

	virtual ~SolibHandler()
	{
		void *rv;

		m_threadShouldExit = true;
		if (m_solibPath != "")
			unlink(m_solibPath.c_str());
		if (m_solibFd >= 0)
			close(m_solibFd);

		/*
		 * First kill the solib thread if it's stuck in open (i.e., before
		 * the tracee has started), then wait for it to terminate for maximum
		 * niceness.
		 *
		 * Only do this if it has been started, naturally
		 */
		if (m_solibThreadValid) {
			pthread_cancel(m_solibThread);
			pthread_kill(m_solibThread, SIGTERM);
			pthread_join(m_solibThread, &rv);
		}
	}

	// From IEventTickListener
	void onTick()
	{
		checkSolibData();
	}


	void startup()
	{
		std::string kcov_solib_pipe_path =
				IOutputHandler::getInstance().getOutDirectory() +
				"kcov-solib.pipe";
		std::string kcov_solib_path =
				IOutputHandler::getInstance().getBaseDirectory() +
				"libkcov_sowrapper.so";

		write_file(__library_data.data(), __library_data.size(), "%s", kcov_solib_path.c_str());

		std::string kcov_solib_env = "KCOV_SOLIB_PATH=" +
				kcov_solib_pipe_path;
		unlink(kcov_solib_pipe_path.c_str());
		(void)mkfifo(kcov_solib_pipe_path.c_str(), 0644);

		free(m_envString);
		m_envString = (char *)xmalloc(kcov_solib_env.size() + 1);
		strcpy(m_envString, kcov_solib_env.c_str());

		std::string preloadEnv = std::string("LD_PRELOAD=" + kcov_solib_path).c_str();
		free(m_ldPreloadString);
		m_ldPreloadString = (char *)xmalloc(preloadEnv.size() + 1);
		strcpy(m_ldPreloadString, preloadEnv.c_str());

		if (IConfiguration::getInstance().keyAsInt("parse-solibs") &&
				ICapabilities::getInstance().hasCapability("handle-solibs")) {
			if (file_exists(kcov_solib_path))
				putenv(m_ldPreloadString);
		}
		putenv(m_envString);

		m_solibPath = kcov_solib_pipe_path;
		pthread_create(&m_solibThread, NULL,
				SolibHandler::threadStatic, (void *)this);

	}

	void solibThreadParse()
	{
		uint8_t buf[1024 * 1024];

		m_solibFd = ::open(m_solibPath.c_str(), O_RDONLY);
		m_solibThreadValid = true;

		if (m_solibFd < 0)
			return;

		m_phdrListMutex.lock();
		while (1) {
			int r = read(m_solibFd, buf, sizeof(buf));

			// The destructor will close m_solibFd, so we'll exit here in that case
			if (r <= 0)
				break;

			panic_if ((unsigned)r >= sizeof(buf),
					"Too much solib data read");

			struct phdr_data *p = phdr_data_unmarshal(buf);

			if (p) {
				size_t sz = sizeof(struct phdr_data) + p->n_entries * sizeof(struct phdr_data_entry);
				struct phdr_data *cpy = (struct phdr_data*)xmalloc(sz);

				memcpy(cpy, p, sz);

				m_phdrs.push_back(cpy);
			}
			m_solibDataReadSemaphore.notify();
		}
		close(m_solibFd);
		m_phdrListMutex.unlock();

		m_solibDataReadSemaphore.notify();
	}

	void solibThreadMain()
	{
		while (1) {
			if (m_threadShouldExit)
				break;

			solibThreadParse();
		}
	}

	// Wrapper for ptrace
	static void *threadStatic(void *pThis)
	{
		SolibHandler *p = (SolibHandler *)pThis;

		p->solibThreadMain();

		return NULL;
	}

	void checkSolibData()
	{
		struct phdr_data *p = NULL;

		if (!m_parser)
			return;

		m_phdrListMutex.lock();
		if (!m_phdrs.empty()) {
			p = m_phdrs.front();
			m_phdrs.pop_front();
		}
		m_phdrListMutex.unlock();

		if (!p)
			return;

		for (unsigned int i = 0; i < p->n_entries; i++)
		{
			struct phdr_data_entry *cur = &p->entries[i];

			if (strlen(cur->name) == 0)
				continue;

			if (m_foundSolibs.find(cur->name) != m_foundSolibs.end())
				continue;

			m_parser->addFile(cur->name, cur);
			m_parser->parse();

			m_foundSolibs[cur->name] = true;
		}

		free(p);
	}


//private:

	typedef std::list<struct phdr_data *> PhdrList_t;
	typedef std::unordered_map<std::string, bool> FoundSolibsMap_t;

	std::string m_solibPath;
	char *m_ldPreloadString;
	char *m_envString;
	int m_solibFd;
	bool m_solibThreadValid;
	bool m_threadShouldExit;
	pthread_t m_solibThread;
	Semaphore m_solibDataReadSemaphore;
	PhdrList_t m_phdrs;
	FoundSolibsMap_t m_foundSolibs;
	std::mutex m_phdrListMutex;

	IFileParser *m_parser;
};


static SolibHandler *g_handler;
ISolibHandler &kcov::createSolibHandler(IFileParser &parser, ICollector &collector)
{
	// OK, not very nicely encapsulated...
	g_handler = new SolibHandler(parser, collector);

	return *g_handler;
}

void kcov::blockUntilSolibDataRead()
{
	// If it has been started
	if (g_handler->m_solibThreadValid)
		g_handler->m_solibDataReadSemaphore.wait();
}
