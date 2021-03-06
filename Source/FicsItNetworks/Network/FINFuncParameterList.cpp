﻿#include "FINFuncParameterList.h"

#include "FINStructParameterList.h"

FFINFuncParameterList::FFINFuncParameterList(UFunction* Func) {
	Data = FMemory::Malloc(Func->GetStructureSize());
	Func->InitializeStruct(Data);
}

FFINFuncParameterList::FFINFuncParameterList(UFunction* Func, void* Data) : Func(Func), Data(Data) {}
FFINFuncParameterList::FFINFuncParameterList(const FFINFuncParameterList& Other) {
	*this = Other;
}

FFINFuncParameterList::~FFINFuncParameterList() {
	if (Data) {
		Func->DestroyStruct(Data);
		FMemory::Free(Data);
		Data = nullptr;
	}
}

FFINFuncParameterList& FFINFuncParameterList::operator=(const FFINFuncParameterList& Other) {
	if (Data) {
		Func->DestroyStruct(Data);
		if (Other.Data) {
			Data = FMemory::Realloc(Data, Other.Func->GetStructureSize());
		} else {
			FMemory::Free(Data);
			Data = nullptr;
		}
	} else if (Other.Data) {
		Data = FMemory::Malloc(Other.Func->GetStructureSize());
	}
	Func = Other.Func;
	if (Data) {
		Func->InitializeStruct(Data);
		for (TFieldIterator<UProperty> It(Func); It; ++It) {
			It->CopyCompleteValue_InContainer(Data, Other.Data);
		}
	}
	
	return *this;
}

int FFINFuncParameterList::operator>>(FFINValueReader& reader) const {
	return FFINStructParameterList::WriteToReader(Func, Data, reader);
}

bool FFINFuncParameterList::Serialize(FArchive& Ar) {
	UFunction* OldFunc = Func;
	Ar << Func;
	if (Ar.IsLoading()) {
		if (Data) {
			OldFunc->DestroyStruct(Data);
			if (Func) {
				Data = FMemory::Realloc(Data, Func->GetStructureSize());
			} else {
				FMemory::Free(Data);
				Data = nullptr;
			}
		} else if (Func) {
			Data = FMemory::Malloc(Func->GetStructureSize());
		}
		if (Func) Func->InitializeStruct(Data);
	}
	if (Func) {
		for (auto p = TFieldIterator<UProperty>(Func); p; ++p) {
			if (Ar.IsLoading()) p->InitializeValue_InContainer(Data);
			if (auto vp = Cast<UStrProperty>(*p)) Ar << *vp->GetPropertyValuePtr_InContainer(Data);
			else if (auto vp = Cast<UIntProperty>(*p)) Ar << *vp->GetPropertyValuePtr_InContainer(Data);
			else if (auto vp = Cast<UInt64Property>(*p)) Ar << *vp->GetPropertyValuePtr_InContainer(Data);
			else if (auto vp = Cast<UFloatProperty>(*p)) Ar << *vp->GetPropertyValuePtr_InContainer(Data);
			else if (auto vp = Cast<UBoolProperty>(*p)) {
				bool b = vp->GetPropertyValue_InContainer(Data);
				Ar << b;
				vp->SetPropertyValue_InContainer(Data, b);
			} else if (auto vp = Cast<UObjectProperty>(*p)) Ar << *vp->GetPropertyValuePtr_InContainer(Data);
			//else if (auto vp = Cast<UArrayProperty>(dp)) reader << vp->GetPropertyValue_InContainer(data);
			// TODO: Add Array support
		}
		//Func->SerializeBin(Ar, Data);
	}
	return true;
}
