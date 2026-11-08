#!/bin/zsh
# Writes DroidliftSigning.xcodeproj: an empty iOS app with
# the game's bundle id and the free team, signed automatically. It exists
# only so Xcode issues and renews the free team's provisioning profile, which
# Apple gives out only through Xcode; ios/sign-device.sh then signs the real
# app (built by CMake) with that profile. Rewritten each time, so its
# settings always match device.local.env.
# usage: ios/signing-project.sh DIR BUNDLE_ID TEAM_ID [IOS_MIN]
#   DL_IOS_MORE_MEMORY=1 also asks for the Increased Memory Limit
#   capability (free teams may have it). Off by default.
set -euo pipefail
DIR=$1 BUNDLE_ID=$2 TEAM_ID=$3 IOS_MIN=${4:-18.0}
P=$DIR/DroidliftSigning.xcodeproj
mkdir -p "$P/xcshareddata/xcschemes" "$DIR/DroidliftSigning"

print -r -- '/* Never runs: the real app is built by CMake. */
int main(void) { return 0; }' > "$DIR/DroidliftSigning/main.c"

ENTITLEMENTS_SETTING=
if [[ ${DL_IOS_MORE_MEMORY:-0} == 1 ]]; then
  cat > "$DIR/DroidliftSigning/DroidliftSigning.entitlements" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.developer.kernel.increased-memory-limit</key>
	<true/>
</dict>
</plist>
EOF
  ENTITLEMENTS_SETTING='CODE_SIGN_ENTITLEMENTS = DroidliftSigning/DroidliftSigning.entitlements;'
else
  rm -f "$DIR/DroidliftSigning/DroidliftSigning.entitlements"
fi

# The target's settings, shared by Debug and Release.
TARGET_SETTINGS="CODE_SIGN_STYLE = Automatic;
				$ENTITLEMENTS_SETTING
				CURRENT_PROJECT_VERSION = 1;
				DEVELOPMENT_TEAM = $TEAM_ID;
				GENERATE_INFOPLIST_FILE = YES;
				INFOPLIST_KEY_CFBundleDisplayName = Signing;
				INFOPLIST_KEY_UILaunchScreen_Generation = YES;
				IPHONEOS_DEPLOYMENT_TARGET = $IOS_MIN;
				MARKETING_VERSION = 1.8.6;
				PRODUCT_BUNDLE_IDENTIFIER = \"$BUNDLE_ID\";
				PRODUCT_NAME = DroidliftSigning;
				SDKROOT = iphoneos;
				SUPPORTED_PLATFORMS = iphoneos;
				SUPPORTS_MACCATALYST = NO;
				TARGETED_DEVICE_FAMILY = 1;"

cat > "$P/project.pbxproj" <<EOF
// !\$*UTF8*\$!
{
	archiveVersion = 1;
	classes = {
	};
	objectVersion = 56;
	objects = {

/* Begin PBXBuildFile section */
		A1000000000000000000000B /* main.c in Sources */ = {isa = PBXBuildFile; fileRef = A1000000000000000000000A /* main.c */; };
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
		A1000000000000000000000A /* main.c */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = main.c; sourceTree = "<group>"; };
		A1000000000000000000000C /* DroidliftSigning.app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = DroidliftSigning.app; sourceTree = BUILT_PRODUCTS_DIR; };
/* End PBXFileReference section */

/* Begin PBXFrameworksBuildPhase section */
		A1000000000000000000000D /* Frameworks */ = {
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
			);
			runOnlyForDeploymentPostprocessing = 0;
		};
/* End PBXFrameworksBuildPhase section */

/* Begin PBXGroup section */
		A10000000000000000000001 = {
			isa = PBXGroup;
			children = (
				A10000000000000000000002 /* DroidliftSigning */,
				A10000000000000000000003 /* Products */,
			);
			sourceTree = "<group>";
		};
		A10000000000000000000002 /* DroidliftSigning */ = {
			isa = PBXGroup;
			children = (
				A1000000000000000000000A /* main.c */,
			);
			path = DroidliftSigning;
			sourceTree = "<group>";
		};
		A10000000000000000000003 /* Products */ = {
			isa = PBXGroup;
			children = (
				A1000000000000000000000C /* DroidliftSigning.app */,
			);
			name = Products;
			sourceTree = "<group>";
		};
/* End PBXGroup section */

/* Begin PBXNativeTarget section */
		A10000000000000000000010 /* DroidliftSigning */ = {
			isa = PBXNativeTarget;
			buildConfigurationList = A10000000000000000000020 /* Build configuration list for PBXNativeTarget "DroidliftSigning" */;
			buildPhases = (
				A1000000000000000000000E /* Sources */,
				A1000000000000000000000D /* Frameworks */,
			);
			buildRules = (
			);
			dependencies = (
			);
			name = DroidliftSigning;
			productName = DroidliftSigning;
			productReference = A1000000000000000000000C /* DroidliftSigning.app */;
			productType = "com.apple.product-type.application";
		};
/* End PBXNativeTarget section */

/* Begin PBXProject section */
		A10000000000000000000000 /* Project object */ = {
			isa = PBXProject;
			attributes = {
				BuildIndependentTargetsInParallel = 1;
				LastUpgradeCheck = 2710;
				TargetAttributes = {
					A10000000000000000000010 = {
						DevelopmentTeam = $TEAM_ID;
						ProvisioningStyle = Automatic;
					};
				};
			};
			buildConfigurationList = A10000000000000000000021 /* Build configuration list for PBXProject "DroidliftSigning" */;
			compatibilityVersion = "Xcode 14.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (
				en,
				Base,
			);
			mainGroup = A10000000000000000000001;
			productRefGroup = A10000000000000000000003 /* Products */;
			projectDirPath = "";
			projectRoot = "";
			targets = (
				A10000000000000000000010 /* DroidliftSigning */,
			);
		};
/* End PBXProject section */

/* Begin PBXSourcesBuildPhase section */
		A1000000000000000000000E /* Sources */ = {
			isa = PBXSourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
				A1000000000000000000000B /* main.c in Sources */,
			);
			runOnlyForDeploymentPostprocessing = 0;
		};
/* End PBXSourcesBuildPhase section */

/* Begin XCBuildConfiguration section */
		A10000000000000000000030 /* Debug */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				$TARGET_SETTINGS
			};
			name = Debug;
		};
		A10000000000000000000031 /* Release */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				$TARGET_SETTINGS
			};
			name = Release;
		};
		A10000000000000000000032 /* Debug */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				ALWAYS_SEARCH_USER_PATHS = NO;
				ONLY_ACTIVE_ARCH = YES;
				SDKROOT = iphoneos;
			};
			name = Debug;
		};
		A10000000000000000000033 /* Release */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				ALWAYS_SEARCH_USER_PATHS = NO;
				SDKROOT = iphoneos;
			};
			name = Release;
		};
/* End XCBuildConfiguration section */

/* Begin XCConfigurationList section */
		A10000000000000000000020 /* Build configuration list for PBXNativeTarget "DroidliftSigning" */ = {
			isa = XCConfigurationList;
			buildConfigurations = (
				A10000000000000000000030 /* Debug */,
				A10000000000000000000031 /* Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Debug;
		};
		A10000000000000000000021 /* Build configuration list for PBXProject "DroidliftSigning" */ = {
			isa = XCConfigurationList;
			buildConfigurations = (
				A10000000000000000000032 /* Debug */,
				A10000000000000000000033 /* Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Debug;
		};
/* End XCConfigurationList section */
	};
	rootObject = A10000000000000000000000 /* Project object */;
}
EOF

cat > "$P/xcshareddata/xcschemes/DroidliftSigning.xcscheme" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion = "2710" version = "1.7">
   <BuildAction parallelizeBuildables = "YES" buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry buildForTesting = "NO" buildForRunning = "YES" buildForProfiling = "NO" buildForArchiving = "NO" buildForAnalyzing = "NO">
            <BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "A10000000000000000000010" BuildableName = "DroidliftSigning.app" BlueprintName = "DroidliftSigning" ReferencedContainer = "container:DroidliftSigning.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
</Scheme>
EOF
echo "$P"
