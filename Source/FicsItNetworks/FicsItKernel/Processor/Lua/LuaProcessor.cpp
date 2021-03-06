#include "LuaProcessor.h"

#include <chrono>

#include "../../FicsItKernel.h"

#include "LuaInstance.h"
#include "LuaStructs.h"
#include "LuaComponentAPI.h"
#include "LuaEventAPI.h"
#include "LuaFileSystemAPI.h"
#include "LuaComputerAPI.h"
#include "LuaProcessorStateStorage.h"

#include "FINStateEEPROMLua.h"
#include "LuaDebugAPI.h"
#include "Network/FINNetworkTrace.h"

#include "SML/util/Logging.h"

namespace FicsItKernel {
	namespace Lua {
		LuaValueReader::LuaValueReader(lua_State* L) : L(L) {}

		void LuaValueReader::nil() {
			lua_pushnil(L);
		}

		void LuaValueReader::operator<<(const FString& str) {
			lua_pushlstring(L, TCHAR_TO_UTF8(*str), str.Len());
		}

		void LuaValueReader::operator<<(FINBool b) {
			lua_pushboolean(L, b);
		}

		void LuaValueReader::operator<<(FINInt num) {
			lua_pushinteger(L, num);
		}
		
		void LuaValueReader::operator<<(FINFloat num) {
			lua_pushnumber(L, num);
		}

		void LuaValueReader::operator<<(FINClass clazz) {
			newInstance(L, clazz);
		}

		void LuaValueReader::operator<<(const FINObj& obj) {
			newInstance(L, FFINNetworkTrace(obj.Get()));
		}

		void LuaValueReader::operator<<(const FINTrace& obj) {
			newInstance(L, obj);
		}

		void LuaValueReader::operator<<(const FINStruct& obj) {
			luaStruct(L, obj);
		}

		void LuaFileSystemListener::onUnmounted(FileSystem::Path path, FileSystem::SRef<FileSystem::Device> device) {
			for (LuaFile file : parent->getFileStreams()) {
				if (file.isValid() && !parent->getKernel()->getFileSystem()->checkUnpersistPath(file->path)) {
					file->file->close();
				}
			}
		}

		void LuaFileSystemListener::onNodeRemoved(FileSystem::Path path, FileSystem::NodeType type) {
			for (LuaFile file : parent->getFileStreams()) {
				if (file.isValid() && file->path.length() > 0 && parent->getKernel()->getFileSystem()->unpersistPath(file->path) == path) {
					file->file->close();
				}
			}
		}

		LuaProcessor* LuaProcessor::luaGetProcessor(lua_State* L) {
			lua_getfield(L, LUA_REGISTRYINDEX, "LuaProcessorPtr");
			LuaProcessor* p = *(LuaProcessor**) luaL_checkudata(L, -1, "LuaProcessor");
			lua_pop(L, 1);
			return p;
		}

		LuaProcessor::LuaProcessor(int speed) : speed(speed), fileSystemListener(new LuaFileSystemListener(this)) {
			
		}

		void LuaProcessor::setKernel(KernelSystem* kernel) {
			if (getKernel() && getKernel()->getFileSystem()) getKernel()->getFileSystem()->removeListener(fileSystemListener);
			Processor::setKernel(kernel);
		}

		void LuaProcessor::tick(float delta) {
			if (!luaState || !luaThread) return;

			// reset out of time
			endOfTick = false;
			lua_sethook(luaThread, luaHook, LUA_MASKCOUNT, speed);
			
			int status = 0;
			if (pullState != 0) {
				// Runtime is pulling a signal
				if (getKernel()->getNetwork()->getSignalCount() > 0) {
					// Signal available -> reset timout and pull signal from network
					pullState = 0;
					int sigArgs = doSignal(luaThread);
					if (sigArgs < 1) {
						// no signals poped -> crash system
						status = LUA_ERRRUN;
					} else {
						// signal poped -> resume yield with signal as parameters (passing signals parameters back to pull yield)
						status = lua_resume(luaThread, nullptr, sigArgs);
					}
				} else if (pullState == 2 || timeout > (static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - pullStart).count()) / 1000.0)) {
					// no signal available & not timeout reached -> skip tick
					return;
				} else {
					// no signal available & timout reached -> resume yield with  no parameters
					pullState = 0;
					status = lua_resume(luaThread, nullptr, 0);
				}
			} else {
				// resume runtime normally
				status = lua_resume(luaThread, nullptr, 0);
			}
			
			if (status == LUA_YIELD) {
				// system yielded and waits for next tick
				lua_gc(luaState, LUA_GCCOLLECT, 0);
				kernel->recalculateResources(KernelSystem::PROCESSOR);
			} else if (status == LUA_OK) {
				// runtime finished execution -> stop system normally
				kernel->stop();
			} else {
				// runtimed crashed -> crash system with runtime error message
				
				luaL_traceback(luaThread, luaThread, lua_tostring(luaThread, -1), 0);
				kernel->crash({ std::string(lua_tostring(luaThread, -1)) });
			}

			// clear some data
			clearFileStreams();
		}

		size_t luaLen(lua_State* L, int idx) {
			size_t len = 0;
			idx = lua_absindex(L, idx);
			lua_pushnil(L);
			while (lua_next(L, idx) != 0) {
				const char* str = lua_tostring(L, -2);
				str = lua_tostring(L, -1);
				len++;
				lua_pop(L, 1);
			}
			return len;
		}

		void LuaProcessor::clearFileStreams() {
			for (auto fs = fileStreams.begin(); fs != fileStreams.end(); ++fs) {
				if (!fs->isValid()) fileStreams.erase(fs--);
			}
		}

		std::set<LuaFile> LuaProcessor::getFileStreams() const {
			return fileStreams;
		}

		void LuaProcessor::reset() {
			// can't reset running system state
			if (getKernel()->getState() != RUNNING) return;

			// reset tempdata
			timeout = -1;
			pullState = 0;
			getKernel()->getFileSystem()->addListener(fileSystemListener);

			// clear existing lua state
			if (luaState) {
				lua_close(luaState);
			}

			// create new lua state
			luaState = luaL_newstate();

			// setup library and perm tables for persistency
			lua_newtable(luaState); // perm
			lua_newtable(luaState); // perm, uperm 

			// register pointer to this Lua Processor in c registry, filestream list & perm table
			luaL_newmetatable(luaState, "LuaProcessor"); // perm, uperm, mt-LuaProcessor
			lua_pop(luaState, 1); // perm, uperm
			LuaProcessor*& luaProcessor = *(LuaProcessor**)lua_newuserdata(luaState, sizeof(LuaProcessor*)); // perm, uperm, proc
			luaL_setmetatable(luaState, "LuaProcessor");
			luaProcessor = this;
			lua_pushvalue(luaState, -1); // perm, uperm, proc, proc
			lua_setfield(luaState, LUA_REGISTRYINDEX, "LuaProcessorPtr"); // perm, uperm, proc
			lua_pushvalue(luaState, -1); // perm, uperm, proc, proc
			lua_setfield(luaState, -3, "LuaProcessor"); // perm, uperm, proc
			lua_pushstring(luaState, "LuaProcessor"); // perm, uperm, proc, "LuaProcessorPtr"
			lua_settable(luaState, -4); // perm, uperm
			luaSetup(luaState); // perm, uperm
			lua_pushlightuserdata(luaState, &fileStreams);
			lua_setfield(luaState, LUA_REGISTRYINDEX, "FileStreamStorage");
			
			// finish perm tables
			lua_setfield(luaState, LUA_REGISTRYINDEX, "PersistUperm"); // perm
			lua_setfield(luaState, LUA_REGISTRYINDEX, "PersistPerm"); //
			
			// create new thread for user code chunk
			luaThread = lua_newthread(luaState);
			luaThreadIndex = lua_gettop(luaState);

			// setup thread with code
			if (!eeprom.IsValid()) {
				kernel->crash(KernelCrash("No Valid EEPROM set"));
				return;
			}
			std::string code = std::string(TCHAR_TO_UTF8(*eeprom->Code), eeprom->Code.Len());
			luaL_loadbuffer(luaThread, code.c_str(), code.size(), "=EEPROM");
			
			// lua_gc(luaState, LUA_GCSETPAUSE, 100);
			// TODO: Check if we actually want to use this or the manual gc call
		}

		std::int64_t LuaProcessor::getMemoryUsage(bool recalc) {
			return lua_gc(luaState, LUA_GCCOUNT, 0)* 1000;
		}

		static constexpr uint32 Base64GetEncodedDataSize(uint32 NumBytes) {
			return ((NumBytes + 2) / 3) * 4;
		}
		
		FString Base64Encode(const uint8* Source, uint32 Length) {
			uint32 ExpectedLength = Base64GetEncodedDataSize(Length);

			FString OutBuffer;

			TArray<TCHAR>& OutCharArray = OutBuffer.GetCharArray();
			OutCharArray.SetNum(ExpectedLength + 1);
			int64 EncodedLength = FBase64::Encode(Source, Length, OutCharArray.GetData());
			verify(EncodedLength == OutBuffer.Len());

			return OutBuffer;
		}

		void LuaProcessor::PreSerialize(UProcessorStateStorage* storage, bool bLoading) {
			for (LuaFile file : fileStreams) {
				if (file->file) {
					file->transfer = FileSystem::SRef<LuaFilePersistTransfer>(new LuaFilePersistTransfer());
					file->transfer->open = file->file->isOpen();
					if (file->transfer->open) {
						file->transfer->mode = file->file->getMode();
						file->transfer->pos = file->file->seek("cur", 0);
					}
					// TODO: Check if combination of APP & TRUNC need special data transfer &
					// TODO: like reading the file store it and then rewrite it to the file when unpersisting.
					file->file->close();
				} else file->transfer = nullptr;
			}
		}

		int luaPersist(lua_State* L) {
			// perm, globals, thread
			
			// persist globals table
			eris_persist(L, 1, 2); // perm, globals, thread, str-globals

			// add global table to perm table
			lua_pushvalue(L, 2); // perm, globals, thread, str-globals, globals
			lua_pushstring(L, "Globals"); // perm, globals, thread, str-globals, globals, "globals"
			lua_settable(L, 1); // perm, globals, thread, str-globals

			// persist thread
			eris_persist(L, 1, 3); // perm, globals, thread, str-globals, str-thread

			return 2;
		}
		
		void LuaProcessor::Serialize(UProcessorStateStorage* storage, bool bLoading) {
			if (!bLoading) {
				// check state & thread
				if (!luaState || !luaThread || lua_status(luaThread) != LUA_YIELD) return;

				ULuaProcessorStateStorage* Data = Cast<ULuaProcessorStateStorage>(storage);

				// save pull state
				Data->PullState = pullState;
				Data->Timeout = timeout;
				Data->PullStart = static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(pullStart.time_since_epoch()).count());
				
				// prepare traces list
				lua_pushlightuserdata(luaState, storage); // ..., storage
				lua_setfield(luaState, LUA_REGISTRYINDEX, "PersistStorage"); // ...
			
				// prepare state data
				lua_getfield(luaState, LUA_REGISTRYINDEX, "PersistPerm"); // ..., perm
				lua_geti(luaState, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS); // ..., perm, globals
				lua_pushvalue(luaState, -1); // ..., perm, globals, globals
				lua_pushnil(luaState); // ..., perm, globals, globals, nil
				lua_settable(luaState, -4); // ..., perm, globals
				lua_pushvalue(luaState, -2); // ..., perm, globals, perm
				lua_pushvalue(luaState, -2); // ..., perm, globals, perm, globals
				lua_pushvalue(luaState, luaThreadIndex); // ..., perm, globals, perm, globals, thread

				lua_pushcfunction(luaState, luaPersist);  // ..., perm, globals, perm, globals, thread, persist-func
				lua_insert(luaState, -4); // ..., perm, globals, persist-func, perm, globals, thread
				int status = lua_pcall(luaState, 3, 2, 0); // ..., perm, globals, str-globals, str-thread

				// check unpersist
				if (status == LUA_OK) {
					// encode persisted globals
					size_t globals_l = 0;
					const char* globals_r = lua_tolstring(luaState, -2, &globals_l);
					Data->Globals = Base64Encode((uint8*)globals_r, globals_l);

					// encode persisted thread
					size_t thread_l = 0;
					const char* thread_r = lua_tolstring(luaState, -1, &thread_l);
					Data->Thread = Base64Encode((uint8*)thread_r, thread_l);
				
					lua_pop(luaState, 2); // ..., perm, globals
				} else {
					// print error
					if (lua_isstring(luaState, -1)) {
						SML::Logging::error("Unable to persit! '", lua_tostring(luaState, -1), "'");
					}

					lua_pop(luaState, 1); // ..., perm, globals
				}

				// cleanup
				lua_pushnil(luaState); // ..., perm, globals, nil
				lua_settable(luaState, -3); // ..., perm
				lua_pop(luaState, 1); // ...
				lua_pushnil(luaState); // ..., nil
				lua_setfield(luaState, LUA_REGISTRYINDEX, "PersistTraces"); // ...
			}
		}

#pragma optimize("", off)
		bool Base64Decode(const FString& Source, TArray<ANSICHAR>& OutData) {
			uint32 ExpectedLength = FBase64::GetDecodedDataSize(Source);

			TArray<ANSICHAR> TempDest;
			TempDest.AddZeroed(ExpectedLength + 1);
			if(!FBase64::Decode(*Source, Source.Len(), (uint8*)TempDest.GetData())) {
				return false;
			}
			OutData = TempDest;
			return true;
		}

		int luaUnpersist(lua_State* L) {
			// str-thread, str-globals, uperm
			// unpersist globals
			eris_unpersist(L, 3, 2); // str-thread, str-globals, uperm, globals

			// add globals to uperm
			lua_pushvalue(L, -1); // str-thread, str-globals, uperm, globals, globals
			lua_setfield(L, 3, "Globals"); // str-thread, str-globals, uperm, globals
			
			// unpersist thread
			eris_unpersist(L, 3, 1); // str-thread, str-globals, uperm, globals, thread
			
			return 2;
		}

		void LuaProcessor::PostSerialize(UProcessorStateStorage* Storage, bool bLoading) {
			if (bLoading) {
				if (kernel->getState() != RUNNING) return;

				ULuaProcessorStateStorage* Data = Cast<ULuaProcessorStateStorage>(Storage);
				
				reset();
				
				// save pull state
				pullState = Data->PullState;
				timeout = Data->Timeout;
				pullStart = std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::milliseconds(Data->PullStart));

				// decode & check data from json
				TArray<ANSICHAR> thread;
				Base64Decode(Data->Thread, thread);
				TArray<ANSICHAR> globals;
				Base64Decode(Data->Globals, globals);
				if (thread.Num() <= 1 && globals.Num() <= 1) return;

				// prepare traces list
				lua_pushlightuserdata(luaState, Storage); // ..., storage
				lua_setfield(luaState, LUA_REGISTRYINDEX, "PersistStorage"); // ...
			
				// get uperm table
				lua_getfield(luaState, LUA_REGISTRYINDEX, "PersistUperm"); // ..., uperm
			
				// prepare protected unpersist
				lua_pushcfunction(luaState, luaUnpersist); // ..., uperm, unpersist

				// push data for protected unpersist
				lua_pushlstring(luaState, thread.GetData(), thread.Num()); // ..., uperm, unpersist, str-thread
				lua_pushlstring(luaState, globals.GetData(), globals.Num()); // ..., uperm, unpersist, str-thread, str-globals
				lua_pushvalue(luaState, -4); // ..., uperm, unpersist, str-thread, str-globals, uperm
			
				// do unpersist
				int ok = lua_pcall(luaState, 3, 2, 0); // ...,  uperm, globals, thread

				// check unpersist
				if (ok != LUA_OK) {
					// print error
					if (lua_isstring(luaState, -1)) {
						SML::Logging::error("Unable to unpersit! '", lua_tostring(luaState, -1), "'");
					}
					
					// cleanup
					lua_pushnil(luaState); // ..., uperm, err, nil
					lua_setfield(luaState, -3, "Globals"); // ..., uperm, err
					lua_pop(luaState, 1); // ...
					lua_pushnil(luaState); // ..., nil
					lua_setfield(luaState, LUA_REGISTRYINDEX, "PersistTraces"); // ...
				
					//throw std::exception("Unable to unpersist");
				} else {
					// cleanup
					lua_pushnil(luaState); // ..., uperm, globals, thread, nil
					lua_setfield(luaState, -4, "Globals"); // ..., uperm, globals, thread
					lua_pushnil(luaState); // ..., uperm, globals, thread, nil
					lua_setfield(luaState, LUA_REGISTRYINDEX, "PersistTraces"); // ..., uperm, globals, thread
				
					// place persisted data
					lua_replace(luaState, luaThreadIndex); // ..., uperm, globals
					lua_seti(luaState, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS); // ..., uperm
					luaThread = lua_tothread(luaState, luaThreadIndex);
					lua_pop(luaState, 1); // ...
				}
			}
		}
#pragma optimize("", on)

		UProcessorStateStorage* LuaProcessor::CreateSerializationStorage() {
			return NewObject<ULuaProcessorStateStorage>();
		}

		void LuaProcessor::setEEPROM(AFINStateEEPROM* eeprom) {
			this->eeprom = Cast<AFINStateEEPROMLua>(eeprom);
			reset();
		}

		int luaReYield(lua_State* L) {
			lua_yield(L,0);
			return 0;
		}

		int luaPrint(lua_State* L) {
			int args = lua_gettop(L);
			std::string log;
			for (int i = 1; i <= args; ++i) {
				size_t s_len = 0;
				const char* s = luaL_tolstring(L, i, &s_len);
				if (!s) luaL_argerror(L, i, "is not valid type");
				log += std::string(s, s_len) + " ";
			}
			if (log.length() > 0) log = log.erase(log.length()-1);
			
			try {
				auto serial = LuaProcessor::luaGetProcessor(L)->getKernel()->getDevDevice()->getSerial()->open(FileSystem::OUTPUT);
				if (serial) {
					*serial << log << "\r\n";
					serial->close();
				}
			} catch (std::exception ex) {
				luaL_error(L, ex.what());
			}
			
			return LuaProcessor::luaAPIReturn(L, 0);
		}

		int luaYieldResume(lua_State* L, int status, lua_KContext ctx) {
			// dont pass pushed bool for user executed yield identification
			return LuaProcessor::luaAPIReturn(L, lua_gettop(L) - 1);
		}

		int luaYield(lua_State* L) {
			int args = lua_gettop(L);

			// insert a boolean to idicate a user executed yield
			lua_pushboolean(L, true);
			lua_insert(L, 1);
			
			return lua_yieldk(L, args+1, NULL, &luaYieldResume);
		}

		int luaResume(lua_State* L); // pre-declare

		int luaResumeResume(lua_State* L, int status, lua_KContext ctx) {
			return LuaProcessor::luaAPIReturn(L, luaResume(L));
		}

		int luaResume(lua_State* L) {
			int args = lua_gettop(L);
			int threadIndex = 1;
			if (lua_isboolean(L, 1)) threadIndex = 2;
			if (!lua_isthread(L, threadIndex)) luaL_argerror(L, threadIndex, "is no thread");
			lua_State* thread = lua_tothread(L, threadIndex);

			// copy passed arguments to coroutine so it can return these arguments from the yield function
			// but dont move the passed coroutine and then resume the coroutine
			lua_xmove(L, thread, args - threadIndex);
			int state = lua_resume(thread, L, args - threadIndex);

			int nargs = lua_gettop(thread);
			// no args indicates return or internal yield (count hook)
			if (nargs == 0) {
				// yield self to cascade the yield down and so the lua execution halts
				if (state == LUA_YIELD) {
					// yield from count hook
					if (threadIndex == 2 && lua_toboolean(L, 1)) {
						return lua_yield(L, 0);
					} else {
						return lua_yieldk(L, 0, NULL, &luaResumeResume);
					}
				} else return LuaProcessor::luaAPIReturn(L, 0);
			}
			
			if (state == LUA_YIELD) nargs -= 1; // remove bool added by overwritten yield

			// copy the parameters passed to yield or returned to our stack so we can return them
			lua_xmove(thread, L, nargs);
			return LuaProcessor::luaAPIReturn(L, nargs);
		}

		void LuaProcessor::luaSetup(lua_State* L) {
			PersistSetup("LuaProcessor", -2);

			luaL_requiref(L, "_G", luaopen_base, true);
			lua_pushnil(L);
			lua_setfield(L, -2, "collectgarbage");
			lua_pushnil(L);
			lua_setfield(L, -2, "dofile");
			lua_pushnil(L);
			lua_setfield(L, -2, "loadfile");
			lua_pushnil(L);
			lua_setfield(L, -2, "setmetatable");
			PersistTable("global", -1);
			lua_pop(L, 1);
			luaL_requiref(L, "table", luaopen_table, true);
			PersistTable("table", -1);
			lua_pop(L, 1);
			luaL_requiref(L, "coroutine", luaopen_coroutine, true);
			lua_pushcfunction(L, luaResume);
			lua_setfield(L, -2, "resume");
			lua_pushcfunction(L, luaYield);
			lua_setfield(L, -2, "yield");
			PersistTable("coroutine", -1);
			lua_register(L, "print", luaPrint);
			PersistGlobal("print");
			lua_pop(L, 1);
			
			luaL_requiref(L, "math", luaopen_math, true);
			PersistTable("math", -1);
			lua_pop(L, 1);
			luaL_requiref(L, "string", luaopen_string, true);
			PersistTable("string", -1);
			lua_pop(L, 1);
			
			setupInstanceSystem(L);
			FFINLuaStructRegistry::Get().Setup(L);
			setupComponentAPI(L);
			setupEventAPI(L);
			setupFileSystemAPI(L);
			setupComputerAPI(L);
			setupDebugAPI(L);
		}

		AFINStateEEPROMLua* LuaProcessor::getEEPROM() {
			return eeprom.Get();
		}

		int LuaProcessor::doSignal(lua_State* L) {
			auto net = getKernel()->getNetwork();
			if (!net || net->getSignalCount() < 1) return 0;
			FFINNetworkTrace sender;
			TFINDynamicStruct<FFINSignal> signal = net->popSignal(sender);
			if (!signal.GetData()) return 0;
			int props = 2;
			lua_pushstring(L, TCHAR_TO_UTF8(*signal->GetName()));
			newInstance(L, sender);
			LuaValueReader reader(L);
			props += signal.Get<FFINSignal>() >> reader;
			return props;
		}

		void LuaProcessor::luaHook(lua_State* L, lua_Debug* ar) {
			LuaProcessor* p = LuaProcessor::luaGetProcessor(L);
			if (p->endOfTick) {
				luaL_error(L, "out of time");
			} else {
				p->endOfTick = true;
				lua_sethook(p->luaThread, luaHook, LUA_MASKCOUNT, p->speed / 2);
			}
		}

		int luaAPIReturn_Resume(lua_State* L, int status, lua_KContext ctx) {
			return static_cast<int>(ctx);
		}

#pragma optimize("", off)
		int LuaProcessor::luaAPIReturn(lua_State* L, int args) {
			LuaProcessor* p = LuaProcessor::luaGetProcessor(L);
			if (p->endOfTick) {
				return lua_yieldk(L, 0, args, &luaAPIReturn_Resume);
			} else {
				return args;
			}
		}
#pragma optimize("", on)
	}
}
