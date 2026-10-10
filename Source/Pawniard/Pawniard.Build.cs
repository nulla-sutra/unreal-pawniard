// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class Pawniard : ModuleRules
{
	public Pawniard(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[]
			{
				Path.Combine(ModuleDirectory, "Animation")
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
				"GameplayTags",
				"AnimGraphRuntime",
				"AnimationWarpingRuntime",
				"AnimationCore"
			}
		);


		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"Slate",
				"SlateCore", "AnimationWarpingRuntime",
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
