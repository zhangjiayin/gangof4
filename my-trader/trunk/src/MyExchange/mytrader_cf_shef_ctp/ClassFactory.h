﻿#pragma once

#include <string>
#include "engine.h"
#include "strategy_unit.h"
#include "quote_entity.h"

typedef strategy_unit<CDepthMarketDataField,MYShfeMarketData,SHFEQuote,MDTenEntrust_MY,MDOrderStatistic_MY> StrategyT;
