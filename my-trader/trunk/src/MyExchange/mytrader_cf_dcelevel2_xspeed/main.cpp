// main.cpp : �������̨Ӧ�ó������ڵ㡣
//

#include <functional>   // std::bind
#include <thread>
#include <stdio.h>      /* printf, tmpnam, L_tmpnam, remove */
#include <stdlib.h>     /* exit */
#include <signal.h>     /* signal */
#include <string>
#include "engine.h"
#include "ClassFactory.h"
#include "../catch_sigs.h"
#include "my_cmn_log.h"
#include "../catch_sigs.h"
#include "maint.h"

using namespace trading_engine;
using namespace std;

typedef engine<CFfexFtdcDepthMarketData,MDBestAndDeep_MY,MDTenEntrust_MY,CDepthMarketDataField,MDOrderStatistic_MY> EngineT;

EngineT *engine_ins;

static void
SIGINT_handler(int s)
{
	LOG4CXX_INFO(log4cxx::Logger::getRootLogger(),"delete engine...");

	engine_ins->finalize();
	delete engine_ins;

	LOG4CXX_INFO(log4cxx::Logger::getRootLogger(),"delete engine.");

	exit(0);		/* call exit for the signal */
}

int main()
{
	(void) my_cmn::my_log::instance(NULL);

	// Load configuration file
	DOMConfigurator::configure("log4cxx_config.xml");

    /*
	struct sched_param param;
	param.sched_priority = 99;
	pid_t pid = getpid();
	sched_setscheduler(pid,SCHED_RR,&param);
    */

	struct sigaction SIGINT_act;
	SIGINT_act.sa_handler = SIGINT_handler;
	sigemptyset(&SIGINT_act.sa_mask);
	SIGINT_act.sa_flags = 0;
	sigaction(SIGINT, &SIGINT_act, NULL);

	 install_sig_handlers();
    maintenance::assemble();

	engine_ins = new EngineT();
	engine_ins->initialize();
	thread t = thread(bind(&EngineT::run,engine_ins));
	t.join();
	
	return 0;
}

