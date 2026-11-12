// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Services/UMetaHumanObjectService.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "Utils/VmhAssets.h"
#include "Utils/VmhJson.h"

namespace
{
	FString PropertyTypeName(const FProperty* Prop)
	{
		if (!Prop)
		{
			return TEXT("?");
		}
		FString Extended;
		FString Base = Prop->GetCPPType(&Extended);
		return Base + Extended;
	}

	TSharedPtr<FJsonObject> DescribeFunction(const UFunction* Function)
	{
		TSharedPtr<FJsonObject> Out = VmhJson::Obj();
		Out->SetStringField(TEXT("name"), Function->GetName());
		Out->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
		Out->SetBoolField(TEXT("blueprint_callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		TArray<TSharedPtr<FJsonValue>> Params;
		FString ReturnType;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnType = PropertyTypeName(*It);
				continue;
			}
			TSharedPtr<FJsonObject> P = VmhJson::Obj();
			P->SetStringField(TEXT("name"), It->GetName());
			P->SetStringField(TEXT("type"), PropertyTypeName(*It));
			P->SetBoolField(TEXT("out"), It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ConstParm));
			Params.Add(MakeShared<FJsonValueObject>(P));
		}
		Out->SetArrayField(TEXT("params"), Params);
		Out->SetStringField(TEXT("returns"), ReturnType.IsEmpty() ? TEXT("void") : ReturnType);
		const FString Tooltip = Function->GetMetaData(TEXT("ToolTip"));
		if (!Tooltip.IsEmpty())
		{
			Out->SetStringField(TEXT("doc"), Tooltip.Left(400));
		}
		return Out;
	}
}

FString UMetaHumanObjectService::GetProperties(const FString& ObjectPath, const TArray<FString>& PropertyNames)
{
	UObject* Object = VmhAssets::Load(ObjectPath);
	if (!Object)
	{
		return VmhJson::Error(TEXT("OBJECT_NOT_FOUND"), FString::Printf(TEXT("No object at %s"), *ObjectPath));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("object"), Object->GetPathName());
	Out->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
	Out->SetObjectField(TEXT("properties"), VmhJson::ObjectToJson(Object, PropertyNames));
	return VmhJson::Ok(Out);
}

FString UMetaHumanObjectService::SetProperties(const FString& ObjectPath, const FString& JsonPatch)
{
	UObject* Object = VmhAssets::Load(ObjectPath);
	if (!Object)
	{
		return VmhJson::Error(TEXT("OBJECT_NOT_FOUND"), FString::Printf(TEXT("No object at %s"), *ObjectPath));
	}
	FString ParseError;
	TSharedPtr<FJsonObject> Patch = VmhJson::Parse(JsonPatch, ParseError);
	if (!Patch.IsValid())
	{
		return VmhJson::Error(TEXT("INVALID_JSON"), ParseError);
	}
	TArray<FString> Changed;
	FString Error;
	if (!VmhJson::PatchObject(Object, Patch, Changed, Error))
	{
		return VmhJson::Error(TEXT("PATCH_FAILED"), Error);
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("object"), Object->GetPathName());
	Out->SetArrayField(TEXT("changed"), VmhJson::StringArray(Changed));
	Out->SetObjectField(TEXT("properties"), VmhJson::ObjectToJson(Object, Changed));
	return VmhJson::Ok(Out);
}

FString UMetaHumanObjectService::CallFunction(const FString& ClassPath, const FString& FunctionName, const FString& ArgsJson)
{
	UClass* Class = VmhJson::FindClass(ClassPath);
	if (!Class)
	{
		return VmhJson::Error(TEXT("CLASS_NOT_FOUND"), FString::Printf(TEXT("No class matches '%s'"), *ClassPath));
	}
	FString ParseError;
	TSharedPtr<FJsonObject> Args = VmhJson::Parse(ArgsJson, ParseError);
	if (!Args.IsValid())
	{
		return VmhJson::Error(TEXT("INVALID_JSON"), ParseError);
	}
	TSharedPtr<FJsonObject> Result;
	FString Error;
	if (!VmhJson::CallStaticFunction(Class, FName(*FunctionName), Args, Result, Error))
	{
		return VmhJson::Error(TEXT("CALL_FAILED"), Error);
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("class"), Class->GetPathName());
	Out->SetStringField(TEXT("function"), FunctionName);
	Out->SetObjectField(TEXT("result"), Result);
	return VmhJson::Ok(Out);
}

FString UMetaHumanObjectService::CallObjectFunction(const FString& ObjectPath, const FString& FunctionName, const FString& ArgsJson)
{
	UObject* Object = VmhAssets::Load(ObjectPath);
	if (!Object)
	{
		return VmhJson::Error(TEXT("OBJECT_NOT_FOUND"), FString::Printf(TEXT("No object at %s"), *ObjectPath));
	}
	FString ParseError;
	TSharedPtr<FJsonObject> Args = VmhJson::Parse(ArgsJson, ParseError);
	if (!Args.IsValid())
	{
		return VmhJson::Error(TEXT("INVALID_JSON"), ParseError);
	}
	TSharedPtr<FJsonObject> Result;
	FString Error;
	if (!VmhJson::CallObjectFunction(Object, FName(*FunctionName), Args, Result, Error))
	{
		return VmhJson::Error(TEXT("CALL_FAILED"), Error);
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("object"), Object->GetPathName());
	Out->SetStringField(TEXT("function"), FunctionName);
	Out->SetObjectField(TEXT("result"), Result);
	return VmhJson::Ok(Out);
}

FString UMetaHumanObjectService::ListFunctions(const FString& ClassPath, const FString& NameFilter)
{
	UClass* Class = VmhJson::FindClass(ClassPath);
	if (!Class)
	{
		return VmhJson::Error(TEXT("CLASS_NOT_FOUND"), FString::Printf(TEXT("No class matches '%s'"), *ClassPath));
	}
	TArray<TSharedPtr<FJsonValue>> Functions;
	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		const UFunction* Function = *It;
		if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_Native))
		{
			continue;
		}
		if (!NameFilter.IsEmpty() && !Function->GetName().Contains(NameFilter))
		{
			continue;
		}
		// Skip UObject plumbing noise
		if (Function->GetOwnerClass() == UObject::StaticClass())
		{
			continue;
		}
		Functions.Add(MakeShared<FJsonValueObject>(DescribeFunction(Function)));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("class"), Class->GetPathName());
	Out->SetNumberField(TEXT("count"), Functions.Num());
	Out->SetArrayField(TEXT("functions"), Functions);
	return VmhJson::Ok(Out);
}

FString UMetaHumanObjectService::DescribeStruct(const FString& StructPath)
{
	UScriptStruct* Struct = VmhJson::FindStruct(StructPath);
	if (!Struct)
	{
		return VmhJson::Error(TEXT("STRUCT_NOT_FOUND"), FString::Printf(TEXT("No struct matches '%s'"), *StructPath));
	}
	TArray<uint8> Defaults;
	Defaults.SetNumZeroed(Struct->GetStructureSize());
	Struct->InitializeStruct(Defaults.GetData());

	TArray<TSharedPtr<FJsonValue>> Fields;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		TSharedPtr<FJsonObject> F = VmhJson::Obj();
		F->SetStringField(TEXT("name"), It->GetName());
		F->SetStringField(TEXT("type"), PropertyTypeName(*It));
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(*It))
		{
			F->SetStringField(TEXT("options"), VmhJson::EnumNames(EnumProp->GetEnum()));
		}
		else if (const FByteProperty* ByteProp = CastField<FByteProperty>(*It))
		{
			if (ByteProp->Enum)
			{
				F->SetStringField(TEXT("options"), VmhJson::EnumNames(ByteProp->Enum));
			}
		}
		const FString Tooltip = It->GetMetaData(TEXT("ToolTip"));
		if (!Tooltip.IsEmpty())
		{
			F->SetStringField(TEXT("doc"), Tooltip.Left(200));
		}
		Fields.Add(MakeShared<FJsonValueObject>(F));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("struct"), Struct->GetPathName());
	Out->SetArrayField(TEXT("fields"), Fields);
	Out->SetObjectField(TEXT("defaults"), VmhJson::StructToJson(Struct, Defaults.GetData()));
	Struct->DestroyStruct(Defaults.GetData());
	return VmhJson::Ok(Out);
}

