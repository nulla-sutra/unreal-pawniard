// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Pawniard : ModuleRules
{
	public Pawniard(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[]
			{
				// ... add public include paths required here ...
			}
		);


		PrivateIncludePaths.AddRange(
			new string[]
			{
				// ... add other private include paths required here ...
			}
		);


		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				// ... add other public dependencies that you statically link with here ...
				"Mover",
				"NavigationSystem",
				"AIModule",
				"Wynaut",
				"EnhancedInput",
				"GameplayTags"
			}
		);


		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"Slate",
				"SlateCore",
				// ... add private dependencies that you statically link with here ...	
			}
		);


		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
		);
	}
}
