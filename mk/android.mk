# Android adapter (canlin#72, docs/superpowers/specs/2026-09-30-canlin-android-adapter-design.md).
# The pure part (src/android/adapter.c: lifecycle state machine, input mapping, row
# conversion) is built and tested on every target. The device part (src/android/activity.c
# plus the NDK's android_native_app_glue, compiled from the NDK tree like libc, not
# vendored), libcanlin.so and the APK exist only for TARGET=android-armv7:
#
#   make TARGET=android-armv7 apk    ->  build/android-armv7/canlin.apk
#
# No Gradle: aapt2 links the manifest against android.jar, the stripped library is added
# with zip, zipalign, then apksigner signs v1 (JAR, required on API 19) + v2 with a debug
# key generated on demand into build/ (never committed). Sorts before mk/app.mk and the
# fragments it needs: their objects are named with second expansion.
ANDROID_HDR := include/canlin/android.h include/canlin/android_limits.h include/canlin/ui.h \
	include/canlin/ui_limits.h include/canlin/shell.h
ANDROID_PURE_OBJ := $(BUILD)/src/android/adapter.o
LIBOBJ += $(ANDROID_PURE_OBJ)

ANDROID_TEST := $(BUILD)/t/android/test_android
TESTS += $(ANDROID_TEST)

$(ANDROID_PURE_OBJ) $(BUILD)/t/android/test_android.o: override CPPFLAGS += \
	-isystem third_party/nuklear
$(ANDROID_PURE_OBJ) $(BUILD)/t/android/test_android.o: $(ANDROID_HDR)

$(ANDROID_TEST): $(BUILD)/t/android/test_android.o $(ANDROID_PURE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

ifeq ($(TARGET),android-armv7)
ANDROID_SDK ?= $(HOME)/android-sdk
ANDROID_BUILD_TOOLS ?= $(ANDROID_SDK)/build-tools/36.0.0
ANDROID_JAR ?= $(ANDROID_SDK)/platforms/android-35/android.jar
AAPT2 := $(ANDROID_BUILD_TOOLS)/aapt2
ZIPALIGN := $(ANDROID_BUILD_TOOLS)/zipalign
APKSIGNER := $(ANDROID_BUILD_TOOLS)/apksigner
NDK_STRIP := $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
KEYTOOL ?= keytool
ZIP ?= zip

GLUE_DIR := $(ANDROID_NDK)/sources/android/native_app_glue
GLUE_OBJ := $(BUILD)/ndk/android_native_app_glue.o
ACTIVITY_OBJ := $(BUILD)/src/android/activity.o
ANDROID_SO := $(BUILD)/lib/libcanlin.so
APK := $(BUILD)/canlin.apk
APK_STAGE := $(BUILD)/apk
APK_KEYSTORE := $(BUILD)/debug.keystore
APK_MANIFEST := src/android/AndroidManifest.xml

# NDK code is not held to our warning set, like vendored code.
$(GLUE_OBJ): $(GLUE_DIR)/android_native_app_glue.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -Wno-error -I$(GLUE_DIR) -c $< -o $@

$(ACTIVITY_OBJ): override CPPFLAGS += -isystem $(GLUE_DIR) -isystem third_party/nuklear \
	-Ithird_party/lua/src
$(ACTIVITY_OBJ): $(ANDROID_HDR) $(APP_HDR) include/canlin/loop.h

# -u keeps canlin_onCreate, the entry NativeActivity looks up by the manifest's
# android.app.func_name; it calls the glue's ANativeActivity_onCreate.
.SECONDEXPANSION:
$(ANDROID_SO): $(ACTIVITY_OBJ) $(GLUE_OBJ) $(ANDROID_PURE_OBJ) $$(APP_LINK_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -Wl,-soname,libcanlin.so -Wl,--no-undefined \
		-u canlin_onCreate $^ -o $@ -landroid -llog -lm

$(APK_KEYSTORE):
	@mkdir -p $(dir $@)
	$(KEYTOOL) -genkeypair -noprompt -keystore $@ -storepass android -keypass android \
		-alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
		-dname "CN=Android Debug,O=Android,C=US"

$(APK): $(ANDROID_SO) $(APK_MANIFEST) | $(APK_KEYSTORE)
	@rm -rf $(APK_STAGE) && mkdir -p $(APK_STAGE)/lib/armeabi-v7a
	$(AAPT2) link -o $(APK_STAGE)/unaligned.apk -I $(ANDROID_JAR) --manifest $(APK_MANIFEST)
	$(NDK_STRIP) --strip-unneeded -o $(APK_STAGE)/lib/armeabi-v7a/libcanlin.so $(ANDROID_SO)
	cd $(APK_STAGE) && $(ZIP) -q -X unaligned.apk lib/armeabi-v7a/libcanlin.so
	$(ZIPALIGN) -f -p 4 $(APK_STAGE)/unaligned.apk $(APK_STAGE)/aligned.apk
	$(APKSIGNER) sign --ks $(APK_KEYSTORE) --ks-pass pass:android --key-pass pass:android \
		--ks-key-alias androiddebugkey --min-sdk-version 19 --v1-signing-enabled true \
		--v2-signing-enabled true --out $@ $(APK_STAGE)/aligned.apk
	@$(APKSIGNER) verify --min-sdk-version 19 -v $@ | grep -q 'v1 scheme (JAR signing): true' \
		|| { echo "error: $@ has no v1 (JAR) signature, API 19 cannot install it" >&2; \
		     rm -f $@; exit 1; }

apk: $(APK)
else
apk:
	@echo "error: apk needs TARGET=android-armv7" >&2; exit 1
endif
.PHONY: apk
