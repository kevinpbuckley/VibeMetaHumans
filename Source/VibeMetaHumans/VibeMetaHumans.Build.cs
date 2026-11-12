// Copyright Buckley Builds LLC 2026 All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class VibeMetaHumans : ModuleRules
{
	public VibeMetaHumans(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.None;
		bUseUnity = false;
		// Engine patch releases can introduce deprecation warnings in the MetaHuman headers we
		// include; keep them visible as warnings rather than failing the build (same policy as VibeUE).
		bWarningsAsErrors = false;

		if (Target.Configuration == UnrealTargetConfiguration.Debug ||
		    Target.Configuration == UnrealTargetConfiguration.DebugGame ||
		    Target.Configuration == UnrealTargetConfiguration.Development)
		{
			OptimizeCode = CodeOptimization.Never;
		}

		// Optional engine plugins. Each one is compiled to a stub ("feature unavailable") when its
		// .uplugin is missing so the rest of the plugin still builds (mirrors WITH_VIBEUE_FAB).
		string PluginsDir = Path.Combine(EngineDirectory, "Plugins");
		bool bCaptureManagerPresent = File.Exists(Path.Combine(PluginsDir, "VirtualProduction", "CaptureManager",
			"CaptureManagerEditor", "CaptureManagerEditor.uplugin"));
		bool bTakesPresent = File.Exists(Path.Combine(PluginsDir, "VirtualProduction", "Takes", "Takes.uplugin"));
		PrivateDefinitions.Add("WITH_VMH_CAPTURE_MANAGER=" + (bCaptureManagerPresent ? "1" : "0"));
		PrivateDefinitions.Add("WITH_VMH_TAKES=" + (bTakesPresent ? "1" : "0"));

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
				"DeveloperSettings",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Editor plumbing
				"UnrealEd",
				"EditorSubsystem",
				"EditorScriptingUtilities",
				"Projects",                 // IPluginManager
				"AssetRegistry",
				"AssetTools",
				"ContentBrowser",
				"Slate",
				"SlateCore",
				"LevelEditor",
				"MovieScene",
				"MovieSceneTracks",
				"LevelSequence",

				// UE 5.9 native AI toolset registry — exposes services as AICallable tools on Epic's MCP endpoint
				"ToolsetRegistry",
				"ModelContextProtocol",

				// MetaHuman Creator (MetaHumanCharacter plugin)
				"MetaHumanCharacter",
				"MetaHumanCharacterEditor",
				"MetaHumanCharacterPalette",
				"MetaHumanCharacterPaletteEditor",
				"MetaHumanDefaultPipeline",
				"MetaHumanDefaultEditorPipeline",

				// MetaHuman SDK + core tech (face/body state types, capture data, realtime pipeline models)
				"MetaHumanSDKRuntime",
				"MetaHumanSDKEditor",
				"MetaHumanCoreTech",
				"MetaHumanCoreTechLib",
				"MetaHumanCaptureData",
				"MetaHumanPipelineCore",
				"MetaHumanPipeline",        // FHyprsenseNode: the face contour tracker as the Mesh Import tool runs it (2026-10-05)
				"MetaHumanFaceContourTracker", // UMetaHumanFaceContourTrackerAsset (its NNE models)
				"NNE",
				"CaptureDataCore",          // UCaptureData / UFootageCaptureData (CaptureData plugin)
				"RigLogicModule",           // EDNADataLayer, DNA readers
				"InterchangeDNA",           // a skeletal mesh from a DNA (generate_skeletal_mesh_from_dna, 2026-10-04)
				"MaterialEditor",           // UMaterialEditingLibrary (set_material_texture)
				"GeometryCore",             // FDynamicMesh3: the conform target rendered for face tracking; AABB/winding queries (wardrobe)
				"MeshDescription",          // wardrobe service: read/edit skeletal mesh LODs (2026-11-12)
				"StaticMeshDescription",
				"SkeletalMeshDescription",  // FSkeletalMeshAttributes: skin weights
				"AnimationCore",            // UE::AnimationCore::FBoneWeight
				"RenderCore",               // FlushRenderingCommands before a mesh LOD rebuild
				"GeometryFramework",        // UDynamicMeshComponent
				"HairStrandsCore",          // UGroomComponent/UGroomAsset/UGroomBindingAsset (2026-10-07 hair tools)
				"HairStrandsEditor",        // UHairStrandsFactory (import_groom)

				// MetaHuman Animator (MetaHuman plugin)
				"MetaHumanCore",
				"MetaHumanIdentity",
				"MetaHumanPerformance",
				"MetaHumanSpeech2Face",

				// Live Link + MetaHuman Live Link sources
				"LiveLinkInterface",
				"LiveLink",
				"LiveLinkComponents",
				"MetaHumanLiveLinkSource",
				"LiveLinkFaceSource",
				"MetaHumanLocalLiveLinkSource",
			}
		);

		if (bTakesPresent)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "TakesCore", "TakeRecorder" });
		}
		// Capture Manager ingest is reached by reflection (its Blueprint library header is private), so no
		// compile-time dependency is needed; the plugin just has to be enabled at runtime.
	}
}
