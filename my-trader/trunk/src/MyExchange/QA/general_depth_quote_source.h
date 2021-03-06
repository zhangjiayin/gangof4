#ifndef general_depth_quote_source_H_
#define general_depth_quote_source_H_

#include "../my_exchange.h"
#include <list>
#include <cstdlib>
#include <list>
#include <chrono>
#include "quote_source_setting.h"
#include "quote_entity.h"
#include <map>
#include <climits>
#include "pending_quote_dao.h"
#include <condition_variable> // std::condition_variable
#include <memory>
#include <thread>
#include "quoteforwarderagent.h"
#include <vector>
#include <memory>
#include "tbb/include/tbb/spin_mutex.h"
#include "pending_quote_dao.h"

using namespace std::chrono;
using namespace std;
using namespace tbb;



namespace quote_agent{
	/**
	quote_source类封装了与具体的行情源的交互。它通过TCP从行情源接收数据，保存到数据库后通知qa类。
	*/
	template<typename QuoteT>
	class quote_source{
	private:

        /*
        * key:model id
        */
        typedef map<long,bool> QuoteStateT;
        
	public:
		quote_source(quote_source_setting setting);
		~quote_source(void);
		void intialize(void);
		void finalize(void);		

	private:
		bool _subscribed;

		/**
		该字段记录qa模块当前状态，如果为true，则表示该模块处于非工作状态
		*/
		bool stopped;
		quote_forwarder_agent<QuoteT> *_forwarder;
		thread _forwarder_thread;

	public:
		// 处理一个行情数据
		void process_one_quote(const QuoteT *quote);

		void subscribe_to_symbols(SubscribeContracts subscription);

		void OnGTAQuoteData(const QuoteT *quote);

		void subscribe_to_quote(const long &model_id, bool isOption, SubscribeContracts contracts);

		bool match(string &lockup_value,SubscribeContracts &lookup_array);

		// 该字段存储quote_source的配置信息
		quote_source_setting setting_;
		
		condition_variable quote_ready_cv;
		mutex quote_ready_mtx;

		QuoteStateT quote_state;
	};
}

#ifdef FUTURE
	#include "fut_general_depth_quote_sourcedef.h"
#else
	#include "general_depth_quote_sourcedef.h"
#endif

#endif
