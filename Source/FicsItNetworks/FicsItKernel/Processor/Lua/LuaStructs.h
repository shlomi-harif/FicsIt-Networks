#pragma once

#include "FGBuildableRailroadTrack.h"
#include "FGInventoryLibrary.h"
#include "FGRailroadTimeTable.h"

#include "Lua.h"
#include "LuaProcessorStateStorage.h"
#include "FicsItKernel/Processor/FicsItFuture.h"

namespace FicsItKernel {
	namespace Lua {
		typedef int(*FutureRetrieveFunc)(lua_State*, TSharedRef<FDynamicStructHolder>);
		typedef void(*FutureResolveFunc)(TSharedRef<FDynamicStructHolder>, TSharedRef<FDynamicStructHolder>);

#define MakeLuaFuture(...) MakeShared<LuaFutureStruct>(__VA_ARGS__)
		
		class LuaFutureStruct : public FicsItFuture {
		public:
			TSharedPtr<FDynamicStructHolder> InData;
			TSharedPtr<FDynamicStructHolder> OutData;
			FutureResolveFunc ResolveFunc;
			FutureRetrieveFunc RetrieveFunc;
			bool valid = false;

			//LuaFutureStruct(TSharedPtr<FDynamicStructHolder> InDat, UStruct* OutDataStruct, FutureResolveFunc ResolveFunc, FutureRetrieveFunc RetrieveFunc);
			LuaFutureStruct(TSharedPtr<FDynamicStructHolder> InDat, TSharedPtr<FDynamicStructHolder> OutDataStruct, FutureResolveFunc ResolveFunc, FutureRetrieveFunc RetrieveFunc);
			~LuaFutureStruct();

			// Begin FicsItFuture
			void Excecute() override;
			// Eng FicsItFuture

			bool IsValid();
			int Retrieve(lua_State* L);
		};

		typedef TSharedPtr<LuaFutureStruct> LuaFuture;
		
		void luaStruct(lua_State* L, FInventoryItem item);
		void luaStruct(lua_State* L, FItemAmount amount);
		void luaStruct(lua_State* L, FInventoryStack stack);
		void luaTrackGraph(lua_State* L, const Network::NetworkTrace& trace ,int trackID);
		void luaTimeTableStop(lua_State* L, const Network::NetworkTrace& station, float duration);
		void luaFuture(lua_State* L, LuaFuture future);
		
		FTimeTableStop luaGetTimeTableStop(lua_State* L, int index);

		void setupStructs(lua_State* L);
	}
}
