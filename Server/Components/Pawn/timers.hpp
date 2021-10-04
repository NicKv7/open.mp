#pragma once

#include <amx/amx.h>
#include "Manager/Manager.hpp"

struct PawnTimerHandler;

struct PawnTimerImpl : public Singleton<PawnTimerImpl> {
	friend struct PawnTimerHandler;

	int setTimer(const char* callback, Milliseconds interval, bool repeating, AMX* amx);
	int setTimerEx(const char* callback, Milliseconds interval, bool repeating, const char* fmt, AMX* amx, const cell* params);

	bool killTimer(int id) {
		auto res = pool.find(id);
		if (res != pool.end()) {
			ITimer* timer = res->second;
			timer->kill();
			return true;
		}
		return false;
	}

	void killTimers(AMX* amx);

private:
	Pair<size_t, PawnTimerHandler*> newTimer(const char* callback, Milliseconds interval, bool repeating, AMX* amx);
	int newTimerExError(PawnTimerHandler* handler, AMX* amx, int err, StringView message);

	size_t insert(ITimer* timer) {
		bool wrappedOnce = false;
		while (pool.find(idx) != pool.end()) {
			if (idx == UINT_MAX) {
				// No free timer slots
				if (wrappedOnce) {
					return 0;
				}
				// Wrap safely
				idx = 0;
				wrappedOnce = true;
			}
			else {
				++idx;
			}
		}
		pool.emplace(idx, timer);
		size_t lastIdx = idx;
		if (idx == UINT_MAX) {
			idx = 0;
		}
		else {
			++idx;
		}
		return lastIdx;
	}

	bool remove(uint32_t id) {
		return pool.erase(id);
	}

	FlatHashMap<uint32_t, ITimer*> pool;
	uint32_t idx = 1;
};

struct PawnTimerHandler final : TimerTimeOutHandler, PoolIDProvider {
	AMX* amx;
	String callback;
	String fmt;
	DynamicArray<cell> params;
	DynamicArray<cell> data;

	PawnTimerHandler(String callback, AMX* amx) :
		amx(amx),
		callback(callback)
	{}

	void timeout(ITimer& timer) override {
		if (!amx) {
			return;
		}

		const bool hasParams = !fmt.empty();

		// Call it.
		// First copy all the data in to the heap.
		cell ret;
		cell out;
		cell* in;
		int err = AMX_ERR_NONE;
		if (hasParams) {
			// Not enough space in this heap.  Try again later.
			if ((err = amx_Allot(amx, data.size(), &out, &in)) != AMX_ERR_NONE) {
				PawnManager::Get()->core->logLn(LogLevel::Error, "SetTimer(Ex): Not enough space in heap for %s timer: %s", callback.c_str(), aux_StrError(err));
				amx_RaiseError(amx, err);
				return;
			}
			// Push the parameters (many all at once).
			memcpy(in, data.data(), data.size() * sizeof(cell));
			for (size_t i = params.size(); i--; )
			{
				switch (fmt[i])
				{
				case 'a':
				case 's':
				case 'v':
					// Copy the data in to the heap, then push the address relative to DAT.
					amx_Push(amx, out + params[i]);
					break;
				default:
					// Just push the address.
					amx_Push(amx, params[i]);
					break;
				}
			}
		}

		int funcidx;
		// Step 4: Call the function.
		if ((err = amx_FindPublic(amx, callback.c_str(), &funcidx)) == AMX_ERR_NONE && (err = amx_Exec(amx, &ret, funcidx)) == AMX_ERR_NONE)
		{
			if (hasParams) {
				// Step 5: Retrieve reference parameters.
				for (size_t i = 0, len = params.size(); i != len; ++i)
				{
					switch (fmt[i])
					{
					case 'v':
						data[params[i] / sizeof(cell)] = *((char*)in + params[i]);
						break;
					}
				}
			}
		}
		else {
			PawnManager::Get()->core->logLn(LogLevel::Error, "SetTimer(Ex): There was a problem in calling %s: %s", callback.c_str(), aux_StrError(err));
			amx_RaiseError(amx, err);
		}

		if (hasParams) {
			// Dispose of the entire stack at once.
			amx_Release(amx, out);
		}
	}

	void free(ITimer& timer) override {
		PawnTimerImpl::Get()->remove(poolID);
		delete this;
	}
};