// Copyright 2019-Present tarnishablec. All Rights Reserved.

using UnrealBuildTool;

public class PawniardAnimGraph : ModuleRules
{
    public PawniardAnimGraph(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Pawniard",
            "AnimationWarpingRuntime",
            "AnimGraphRuntime",
            "AnimGraph",
            "BlueprintGraph",
            "UnrealEd"
        });
    }
}
