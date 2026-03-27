#include <Geode/Bindings.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/utils/web.hpp>

#include <chrono>
#include <cmath>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr float kPracticeButtonScale = 0.46f;
    constexpr float kBrowseButtonScale = 0.34f;
    constexpr float kUseButtonScale = 0.48f;
    constexpr float kButtonGap = 12.f;
    constexpr float kScreenMargin = 18.f;
    constexpr auto kLookupTimeout = std::chrono::seconds(5);
    constexpr auto kSubmitTimeout = std::chrono::seconds(10);
    constexpr auto kApprovedMappingsCacheTtl = std::chrono::seconds(30);

    constexpr char kDefaultGithubRepository[] = "NotDevNoob/gd-level-mappings";
    constexpr char kDefaultGithubBranch[] = "main";
    constexpr char kDefaultGithubJsonPath[] = "mappings.json";

    struct ApprovedMappingsCache {
        std::optional<matjson::Value> payload;
        std::chrono::steady_clock::time_point fetchedAt {};
    };

    struct PendingSelectionState {
        bool active = false;
        int originalLevelId = 0;
        std::string originalLevelName;
    };

    ApprovedMappingsCache gApprovedMappingsCache;
    PendingSelectionState gPendingSelection;

    std::string trimWhitespace(std::string value) {
        auto isSpace = [](unsigned char character) {
            return std::isspace(character) != 0;
        };

        while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }

    std::string normalizedApiBaseUrl() {
        auto value = trimWhitespace(Mod::get()->getSettingValue<std::string>("api-base-url"));
        while (!value.empty() && value.back() == '/') {
            value.pop_back();
        }
        return value;
    }

    std::string normalizedGithubRepository() {
        auto value = trimWhitespace(Mod::get()->getSettingValue<std::string>("github-repository"));
        return value.empty() ? kDefaultGithubRepository : value;
    }

    std::string normalizedGithubBranch() {
        auto value = trimWhitespace(Mod::get()->getSettingValue<std::string>("github-branch"));
        return value.empty() ? kDefaultGithubBranch : value;
    }

    std::string normalizedGithubJsonPath() {
        auto value = trimWhitespace(Mod::get()->getSettingValue<std::string>("github-json-path"));
        return value.empty() ? kDefaultGithubJsonPath : value;
    }

    bool isValidGithubRepository(std::string const& value) {
        auto slash = value.find('/');
        return slash != std::string::npos && slash > 0 && slash + 1 < value.size();
    }

    std::string buildGithubRawMappingsUrl(std::string const& repository, std::string const& branch, std::string const& jsonPath) {
        if (!isValidGithubRepository(repository) || branch.empty() || jsonPath.empty()) {
            return "";
        }

        return fmt::format(
            "https://raw.githubusercontent.com/{}/{}/{}",
            repository,
            branch,
            jsonPath
        );
    }

    std::string normalizedApprovedMappingsUrl() {
        auto overrideUrl = trimWhitespace(Mod::get()->getSettingValue<std::string>("approved-mappings-url"));
        if (!overrideUrl.empty()) {
            return overrideUrl;
        }

        return buildGithubRawMappingsUrl(
            normalizedGithubRepository(),
            normalizedGithubBranch(),
            normalizedGithubJsonPath()
        );
    }

    std::string withCacheBust(std::string url) {
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        return fmt::format("{}{}pm_ts={}", url, url.find('?') == std::string::npos ? "?" : "&", timestamp);
    }

    bool hasFreshApprovedMappingsCache() {
        return gApprovedMappingsCache.payload.has_value() &&
            (std::chrono::steady_clock::now() - gApprovedMappingsCache.fetchedAt) < kApprovedMappingsCacheTtl;
    }

    void cacheApprovedMappings(matjson::Value const& payload) {
        gApprovedMappingsCache.payload = payload;
        gApprovedMappingsCache.fetchedAt = std::chrono::steady_clock::now();
    }

    std::optional<int> findPracticeLevelIdInMappings(matjson::Value const& payload, int originalLevelId) {
        if (!payload.isObject()) {
            return std::nullopt;
        }

        auto key = fmt::format("{}", originalLevelId);
        if (!payload.contains(key)) {
            return std::nullopt;
        }

        auto rawValue = payload[key];
        if (rawValue.isNumber()) {
            auto parsed = rawValue.asInt().unwrapOr(0);
            return parsed > 0 ? std::optional(parsed) : std::nullopt;
        }

        if (rawValue.isString()) {
            auto parsed = numFromString<int>(rawValue.asString().unwrapOr(""));
            if (parsed && *parsed > 0) {
                return *parsed;
            }
        }

        return std::nullopt;
    }

    bool hasPendingSelection() {
        return gPendingSelection.active && gPendingSelection.originalLevelId > 0;
    }

    void beginPendingSelection(int originalLevelId, std::string originalLevelName) {
        gPendingSelection.active = true;
        gPendingSelection.originalLevelId = originalLevelId;
        gPendingSelection.originalLevelName = std::move(originalLevelName);
    }

    void clearPendingSelection() {
        gPendingSelection = {};
    }

    std::string formatLevelShort(int levelId, std::string const& levelName) {
        if (!levelName.empty()) {
            return fmt::format("{} (#{})", levelName, levelId);
        }
        return fmt::format("#{}", levelId);
    }

    void showNotice(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
        Notification::create(text, icon, 1.2f)->show();
    }

    void showPopup(std::string const& title, std::string const& text) {
        FLAlertLayer::create(title.c_str(), text, "OK")->show();
    }

    std::string submissionErrorMessage(web::WebResponse const& response) {
        auto jsonResult = response.json();
        if (jsonResult) {
            auto payload = jsonResult.unwrap();
            if (payload.isObject() && payload.contains("error") && payload["error"].isString()) {
                return std::string(payload["error"].asString().unwrapOr("Submission failed"));
            }
        }

        auto message = response.errorMessage();
        if (!message.empty()) {
            return std::string(message);
        }

        return fmt::format("HTTP {}", response.code());
    }

    CCRect rectInNodeSpace(CCNode* node, CCNode* relativeTo) {
        auto rect = node->boundingBox();
        auto worldMin = node->getParent()->convertToWorldSpace(rect.origin);
        auto worldMax = node->getParent()->convertToWorldSpace({
            rect.origin.x + rect.size.width,
            rect.origin.y + rect.size.height,
        });
        auto localMin = relativeTo->convertToNodeSpace(worldMin);
        auto localMax = relativeTo->convertToNodeSpace(worldMax);
        return {
            localMin.x,
            localMin.y,
            localMax.x - localMin.x,
            localMax.y - localMin.y,
        };
    }

    void collectInteractiveBounds(CCNode* node, CCNode* relativeTo, CCNode* ignore, std::vector<CCRect>& out) {
        if (!node || !node->isVisible() || node == ignore) {
            return;
        }

        if (typeinfo_cast<CCMenuItem*>(node) != nullptr) {
            out.push_back(rectInNodeSpace(node, relativeTo));
        }

        if (auto children = node->getChildren()) {
            for (auto child : node->getChildrenExt()) {
                collectInteractiveBounds(child, relativeTo, ignore, out);
            }
        }
    }

    bool rectFits(CCRect const& candidate, CCSize const& layerSize, std::vector<CCRect> const& occupied) {
        if (candidate.getMinX() < kScreenMargin || candidate.getMaxX() > layerSize.width - kScreenMargin) {
            return false;
        }

        if (candidate.getMinY() < kScreenMargin || candidate.getMaxY() > layerSize.height - kScreenMargin) {
            return false;
        }

        for (auto const& used : occupied) {
            if (candidate.intersectsRect(used)) {
                return false;
            }
        }

        return true;
    }

    CCRect centeredRect(CCPoint const& center, CCSize const& size) {
        return {
            center.x - size.width / 2.f,
            center.y - size.height / 2.f,
            size.width,
            size.height,
        };
    }
}

class $modify(PracticeMappingLevelInfoLayer, LevelInfoLayer) {
    struct Fields {
        CCMenu* overlayMenu = nullptr;
        CCMenuItemSpriteExtra* practiceButton = nullptr;
        CCMenuItemSpriteExtra* browseButton = nullptr;
        CCMenuItemSpriteExtra* useButton = nullptr;
        CCMenuItemSpriteExtra* cancelButton = nullptr;
        async::TaskHolder<web::WebResponse> mappingRequest;
        async::TaskHolder<web::WebResponse> submitRequest;
        int mappedPracticeLevelId = 0;
        bool lookupInFlight = false;
        bool submitInFlight = false;
    };

    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) {
            return false;
        }

        this->startMappingLookup(false);
        return true;
    }

    void ensureOverlayMenu() {
        if (m_fields->overlayMenu) {
            return;
        }

        auto overlayMenu = CCMenu::create();
        overlayMenu->setID("practice-mapping-menu");
        overlayMenu->setContentSize(this->getContentSize());
        overlayMenu->ignoreAnchorPointForPosition(false);
        overlayMenu->setAnchorPoint({ 0.f, 0.f });
        overlayMenu->setPosition({ 0.f, 0.f });
        this->addChild(overlayMenu, 25);
        m_fields->overlayMenu = overlayMenu;
    }

    void removeButton(CCMenuItemSpriteExtra*& button) {
        if (!button) {
            return;
        }

        button->removeFromParent();
        button = nullptr;
    }

    std::string currentLevelName() const {
        if (!m_level) {
            return "";
        }
        return m_level->m_levelName.empty() ? "" : std::string(m_level->m_levelName.c_str());
    }

    CCMenuItemSpriteExtra* createIconButton(
        char const* spriteFrame,
        float scale,
        float rotation,
        SEL_MenuHandler handler,
        char const* id
    ) {
        auto sprite = CCSprite::createWithSpriteFrameName(spriteFrame);
        if (!sprite) {
            log::warn("Could not load {}", spriteFrame);
            return nullptr;
        }

        sprite->setScale(scale);
        sprite->setRotation(rotation);

        auto button = CCMenuItemSpriteExtra::create(sprite, this, handler);
        button->setID(id);
        return button;
    }

    CCNode* findButtonAnchor() {
        if (m_difficultySprite && m_difficultySprite->isVisible()) {
            return m_difficultySprite;
        }

        auto playButton = this->findPrimaryPlayButton();
        if (playButton && playButton->isVisible()) {
            return playButton;
        }

        return nullptr;
    }

    std::optional<CCPoint> findSafeButtonPosition(CCMenuItemSpriteExtra* button) {
        auto anchorNode = this->findButtonAnchor();
        if (!anchorNode) {
            return std::nullopt;
        }

        auto layerSize = this->getContentSize();
        auto anchorRect = rectInNodeSpace(anchorNode, this);
        auto buttonSize = button->getScaledContentSize();

        std::vector<CCRect> occupied;
        collectInteractiveBounds(this, this, button, occupied);

        std::vector<CCPoint> candidates = {
            { anchorRect.getMidX(), anchorRect.getMinY() - 32.f - buttonSize.height / 2.f },
            { anchorRect.getMidX() - (buttonSize.width + 6.f), anchorRect.getMinY() - 32.f - buttonSize.height / 2.f },
            { anchorRect.getMidX() + (buttonSize.width + 6.f), anchorRect.getMinY() - 32.f - buttonSize.height / 2.f },
            { anchorRect.getMidX(), anchorRect.getMaxY() + 8.f + buttonSize.height / 2.f },
            { anchorRect.getMidX() - (buttonSize.width + 6.f), anchorRect.getMaxY() + 8.f + buttonSize.height / 2.f },
            { anchorRect.getMidX() + (buttonSize.width + 6.f), anchorRect.getMaxY() + 8.f + buttonSize.height / 2.f },
        };

        for (auto const& candidate : candidates) {
            if (rectFits(centeredRect(candidate, buttonSize), layerSize, occupied)) {
                return candidate;
            }
        }

        for (int radius = 1; radius <= 5; ++radius) {
            for (int y = -radius; y <= radius; ++y) {
                for (int x = -radius; x <= radius; ++x) {
                    if (std::abs(x) != radius && std::abs(y) != radius) {
                        continue;
                    }

                    auto candidate = CCPoint {
                        anchorRect.getMidX() + static_cast<float>(x) * (buttonSize.width + kButtonGap),
                        anchorRect.getMidY() + static_cast<float>(y) * (buttonSize.height + kButtonGap),
                    };

                    if (rectFits(centeredRect(candidate, buttonSize), layerSize, occupied)) {
                        return candidate;
                    }
                }
            }
        }

        return std::nullopt;
    }

    void placeButton(CCMenuItemSpriteExtra* button) {
        if (!button) {
            return;
        }

        this->ensureOverlayMenu();
        auto position = this->findSafeButtonPosition(button);
        if (!position) {
            log::warn("Unable to find safe placement for practice mapper button");
            return;
        }

        if (!button->getParent()) {
            m_fields->overlayMenu->addChild(button);
        }
        button->setPosition(*position);
    }

    CCMenuItemSpriteExtra* findPrimaryPlayButton() {
        if (!m_playBtnMenu || !m_playBtnMenu->getChildren()) {
            return nullptr;
        }

        CCMenuItemSpriteExtra* best = nullptr;
        float bestArea = 0.f;

        for (auto node : m_playBtnMenu->getChildrenExt()) {
            auto item = typeinfo_cast<CCMenuItemSpriteExtra*>(node);
            if (!item || !item->isVisible()) {
                continue;
            }

            auto size = item->getScaledContentSize();
            auto area = size.width * size.height;
            if (!best || area > bestArea) {
                best = item;
                bestArea = area;
            }
        }

        return best;
    }

    void setButtonState(CCMenuItemSpriteExtra* button, bool enabled) {
        if (!button) {
            return;
        }

        button->setEnabled(enabled);
        button->setOpacity(enabled ? 255 : 140);
    }

    void updateActionButtonStates() {
        this->setButtonState(
            m_fields->practiceButton,
            !m_fields->lookupInFlight && !m_fields->submitInFlight && m_fields->mappedPracticeLevelId > 0
        );
        this->setButtonState(m_fields->browseButton, !m_fields->submitInFlight);
        this->setButtonState(m_fields->useButton, !m_fields->submitInFlight);
        this->setButtonState(m_fields->cancelButton, !m_fields->submitInFlight);
    }

    void addPracticeButton() {
        if (m_fields->practiceButton) {
            this->placeButton(m_fields->practiceButton);
            this->updateActionButtonStates();
            return;
        }

        auto practiceSprite = CCSprite::createWithSpriteFrameName("GJ_practiceBtn_001.png");
        if (!practiceSprite) {
            log::warn("Could not load GJ_practiceBtn_001.png");
            return;
        }

        practiceSprite->setScale(kPracticeButtonScale);
        m_fields->practiceButton = CCMenuItemSpriteExtra::create(
            practiceSprite,
            this,
            menu_selector(PracticeMappingLevelInfoLayer::onPracticePressed)
        );
        m_fields->practiceButton->setID("practice-mapping-button");
        this->placeButton(m_fields->practiceButton);
        this->updateActionButtonStates();
    }

    void addBrowseButton() {
        if (!m_fields->browseButton) {
            m_fields->browseButton = this->createIconButton(
                "GJ_closeBtn_001.png",
                kBrowseButtonScale,
                45.f,
                menu_selector(PracticeMappingLevelInfoLayer::onBrowsePracticePressed),
                "practice-picker-button"
            );
        }

        if (!m_fields->browseButton) {
            return;
        }

        this->placeButton(m_fields->browseButton);
        this->updateActionButtonStates();
    }

    void addUseButton() {
        if (!m_fields->useButton) {
            m_fields->useButton = this->createIconButton(
                "GJ_checkOn_001.png",
                kUseButtonScale,
                0.f,
                menu_selector(PracticeMappingLevelInfoLayer::onUseSelectedLevelPressed),
                "practice-use-button"
            );
        }

        if (!m_fields->useButton) {
            return;
        }

        this->placeButton(m_fields->useButton);
        this->updateActionButtonStates();
    }

    void addCancelButton() {
        if (!m_fields->cancelButton) {
            m_fields->cancelButton = this->createIconButton(
                "GJ_closeBtn_001.png",
                kBrowseButtonScale,
                0.f,
                menu_selector(PracticeMappingLevelInfoLayer::onCancelSelectionPressed),
                "practice-cancel-button"
            );
        }

        if (!m_fields->cancelButton) {
            return;
        }

        this->placeButton(m_fields->cancelButton);
        this->updateActionButtonStates();
    }

    void refreshActionButtons() {
        auto selectionActive = hasPendingSelection();
        auto currentLevelId = m_level ? m_level->m_levelID.value() : 0;
        auto hasSelectableCandidate = selectionActive &&
            currentLevelId > 0 &&
            currentLevelId != gPendingSelection.originalLevelId;

        if (hasSelectableCandidate) {
            this->removeButton(m_fields->practiceButton);
            this->removeButton(m_fields->browseButton);
            this->addUseButton();
            this->addCancelButton();
            return;
        }

        this->removeButton(m_fields->useButton);
        this->removeButton(m_fields->cancelButton);

        if (m_fields->mappedPracticeLevelId > 0) {
            this->removeButton(m_fields->browseButton);
            this->addPracticeButton();
            return;
        }

        this->removeButton(m_fields->practiceButton);

        if (!m_fields->lookupInFlight) {
            this->addBrowseButton();
            if (selectionActive) {
                this->addCancelButton();
            }
        } else {
            this->removeButton(m_fields->browseButton);
        }
    }

    void finishMappingLookup(std::optional<int> practiceLevelId) {
        m_fields->lookupInFlight = false;
        m_fields->mappedPracticeLevelId = practiceLevelId.value_or(0);
        this->refreshActionButtons();
        this->updateActionButtonStates();
    }

    void startApiLookup() {
        auto apiBaseUrl = normalizedApiBaseUrl();
        if (apiBaseUrl.empty()) {
            this->finishMappingLookup(std::nullopt);
            return;
        }

        auto originalLevelId = m_level ? m_level->m_levelID.value() : 0;
        if (originalLevelId <= 0) {
            this->finishMappingLookup(std::nullopt);
            return;
        }

        m_fields->lookupInFlight = true;
        this->refreshActionButtons();

        auto url = fmt::format("{}/mapping?level_id={}", apiBaseUrl, originalLevelId);
        web::WebRequest request;
        request.timeout(kLookupTimeout);
        m_fields->mappingRequest.spawn(
            "Practice mapping API lookup",
            request.get(url),
            [this](web::WebResponse response) {
                this->onApiMappingResponse(std::move(response));
            }
        );
    }

    void applyApprovedMappingsLookup(matjson::Value const& payload) {
        auto originalLevelId = m_level ? m_level->m_levelID.value() : 0;
        if (originalLevelId <= 0) {
            this->finishMappingLookup(std::nullopt);
            return;
        }

        auto practiceLevelId = findPracticeLevelIdInMappings(payload, originalLevelId);
        if (practiceLevelId) {
            this->finishMappingLookup(practiceLevelId);
            return;
        }

        if (!normalizedApiBaseUrl().empty()) {
            this->startApiLookup();
            return;
        }

        this->finishMappingLookup(std::nullopt);
    }

    void startMappingLookup(bool) {
        if (!m_level || m_fields->lookupInFlight) {
            return;
        }

        auto originalLevelId = m_level->m_levelID.value();
        if (originalLevelId <= 0) {
            return;
        }

        m_fields->mappedPracticeLevelId = 0;

        if (hasFreshApprovedMappingsCache()) {
            this->applyApprovedMappingsLookup(*gApprovedMappingsCache.payload);
            return;
        }

        auto mappingsUrl = normalizedApprovedMappingsUrl();
        if (mappingsUrl.empty()) {
            this->startApiLookup();
            return;
        }

        m_fields->lookupInFlight = true;
        this->refreshActionButtons();

        web::WebRequest request;
        request.timeout(kLookupTimeout);
        m_fields->mappingRequest.spawn(
            "Approved practice mappings lookup",
            request.get(withCacheBust(mappingsUrl)),
            [this](web::WebResponse response) {
                this->onApprovedMappingsResponse(std::move(response));
            }
        );
    }

    void onApprovedMappingsResponse(web::WebResponse response) {
        if (response.cancelled()) {
            this->finishMappingLookup(std::nullopt);
            return;
        }

        if (!response.ok()) {
            log::warn(
                "Approved mappings lookup failed with code {}: {}",
                response.code(),
                response.errorMessage()
            );
            this->startApiLookup();
            return;
        }

        auto jsonResult = response.json();
        if (!jsonResult) {
            log::warn("Approved mappings lookup returned invalid JSON: {}", jsonResult.unwrapErr());
            this->startApiLookup();
            return;
        }

        auto payload = jsonResult.unwrap();
        if (!payload.isObject()) {
            log::warn("Approved mappings lookup returned an unexpected payload: {}", payload.dump());
            this->startApiLookup();
            return;
        }

        cacheApprovedMappings(payload);
        this->applyApprovedMappingsLookup(payload);
    }

    void onApiMappingResponse(web::WebResponse response) {
        if (response.cancelled()) {
            this->finishMappingLookup(std::nullopt);
            return;
        }

        if (!response.ok()) {
            log::warn("Practice mapping API lookup failed with code {}: {}", response.code(), response.errorMessage());
            this->finishMappingLookup(std::nullopt);
            return;
        }

        auto jsonResult = response.json();
        if (!jsonResult) {
            log::warn("Practice mapping API lookup returned invalid JSON: {}", jsonResult.unwrapErr());
            this->finishMappingLookup(std::nullopt);
            return;
        }

        auto payload = jsonResult.unwrap();
        if (!payload.isObject() || !payload.contains("found") || !payload["found"].isBool()) {
            log::warn("Practice mapping API lookup returned an unexpected payload: {}", payload.dump());
            this->finishMappingLookup(std::nullopt);
            return;
        }

        if (!payload["found"].asBool().unwrapOr(false)) {
            this->finishMappingLookup(std::nullopt);
            return;
        }

        if (!payload.contains("practiceLevelId") || !payload["practiceLevelId"].isNumber()) {
            log::warn("Practice mapping payload is missing practiceLevelId: {}", payload.dump());
            this->finishMappingLookup(std::nullopt);
            return;
        }

        auto practiceLevelId = payload["practiceLevelId"].asInt().unwrapOr(0);
        this->finishMappingLookup(practiceLevelId > 0 ? std::optional(practiceLevelId) : std::nullopt);
    }

    void onPracticePressed(CCObject*) {
        if (m_fields->lookupInFlight || m_fields->submitInFlight || m_fields->mappedPracticeLevelId <= 0) {
            return;
        }

        this->openPracticeLevel(m_fields->mappedPracticeLevelId);
    }

    void onBrowsePracticePressed(CCObject*) {
        if (!m_level || m_fields->submitInFlight) {
            return;
        }

        auto originalLevelId = m_level->m_levelID.value();
        if (originalLevelId <= 0) {
            showPopup("Practice Mapper", "This level does not have a valid online ID.");
            return;
        }

        auto originalLevelName = this->currentLevelName();
        auto popup = createQuickPopup(
            "Pick Practice Level",
            "Open the normal search screen, find the practice level you want, open it, and press <cg>Use This Level</c>.",
            "Cancel",
            "Open Search",
            [this, originalLevelId, originalLevelName](auto, bool openSearch) {
                if (!openSearch) {
                    return;
                }

                beginPendingSelection(originalLevelId, originalLevelName);

                auto scene = LevelSearchLayer::scene(0);
                if (!scene) {
                    clearPendingSelection();
                    showPopup("Practice Mapper", "Could not open the level search screen.");
                    return;
                }

                showNotice("Choose a practice level, open it, then press Use This Level.");
                CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.25f, scene));
            },
            false
        );
        popup->m_noElasticity = true;
        popup->show();
    }

    void onUseSelectedLevelPressed(CCObject*) {
        if (!m_level || m_fields->submitInFlight || !hasPendingSelection()) {
            return;
        }

        auto practiceLevelId = m_level->m_levelID.value();
        if (practiceLevelId <= 0) {
            showPopup("Practice Mapper", "This level does not have a valid online ID.");
            return;
        }

        if (practiceLevelId == gPendingSelection.originalLevelId) {
            showPopup("Practice Mapper", "Pick a different level for the practice mapping.");
            return;
        }

        auto confirmText = fmt::format(
            "Submit <cy>{}</c> -> <cg>{}</c> for Discord review?",
            formatLevelShort(gPendingSelection.originalLevelId, gPendingSelection.originalLevelName),
            formatLevelShort(practiceLevelId, this->currentLevelName())
        );

        auto popup = createQuickPopup(
            "Submit Mapping",
            confirmText,
            "Cancel",
            "Submit",
            [this](auto, bool shouldSubmit) {
                if (shouldSubmit) {
                    this->submitPendingSelection();
                }
            },
            false
        );
        popup->m_noElasticity = true;
        popup->show();
    }

    void onCancelSelectionPressed(CCObject*) {
        if (!hasPendingSelection()) {
            return;
        }

        clearPendingSelection();
        this->refreshActionButtons();
        showNotice("Practice level selection cancelled.");
    }

    void submitPendingSelection() {
        if (!m_level || m_fields->submitInFlight || !hasPendingSelection()) {
            return;
        }

        auto apiBaseUrl = normalizedApiBaseUrl();
        if (apiBaseUrl.empty()) {
            showPopup(
                "Practice Mapper",
                "Set API Base URL in the mod settings before submitting mappings from inside the game."
            );
            return;
        }

        auto practiceLevelId = m_level->m_levelID.value();
        if (practiceLevelId <= 0 || practiceLevelId == gPendingSelection.originalLevelId) {
            showPopup("Practice Mapper", "Pick a different valid online level for the practice mapping.");
            return;
        }

        auto payloadResult = matjson::parse(fmt::format(
            R"({{"originalLevelId":{},"practiceLevelId":{},"submittedBy":{{"username":"Geometry Dash Mod"}}}})",
            gPendingSelection.originalLevelId,
            practiceLevelId
        ));
        if (!payloadResult) {
            showNotice("Could not build the submission request", NotificationIcon::Warning);
            return;
        }

        m_fields->submitInFlight = true;
        this->updateActionButtonStates();

        web::WebRequest request;
        request.timeout(kSubmitTimeout);
        request.bodyJSON(payloadResult.unwrap());
        m_fields->submitRequest.spawn(
            "Practice mapping submission",
            request.post(fmt::format("{}/submit", apiBaseUrl)),
            [this](web::WebResponse response) {
                this->onSubmitResponse(std::move(response));
            }
        );
    }

    void onSubmitResponse(web::WebResponse response) {
        m_fields->submitInFlight = false;
        this->updateActionButtonStates();

        if (response.cancelled()) {
            return;
        }

        if (!response.ok()) {
            showPopup("Submission Failed", submissionErrorMessage(response));
            return;
        }

        auto originalLevelId = gPendingSelection.originalLevelId;
        auto originalLevelName = gPendingSelection.originalLevelName;
        auto practiceLevelId = m_level ? m_level->m_levelID.value() : 0;
        auto practiceLevelName = this->currentLevelName();

        clearPendingSelection();
        m_fields->mappedPracticeLevelId = 0;
        this->refreshActionButtons();

        showPopup(
            "Queued For Review",
            fmt::format(
                "{} -> {} was queued for Discord review. If the bot is offline, it will appear in the approvals channel when the bot comes back online.",
                formatLevelShort(originalLevelId, originalLevelName),
                formatLevelShort(practiceLevelId, practiceLevelName)
            )
        );
    }

    void openPracticeLevel(int practiceLevelId) {
        auto manager = GameLevelManager::get();
        if (!manager) {
            showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            return;
        }

        if (auto savedLevel = manager->getSavedLevel(practiceLevelId)) {
            auto resolvedLevelId = savedLevel->m_levelID.value();
            if (resolvedLevelId == practiceLevelId) {
                this->switchToMappedLevel(savedLevel);
                return;
            }

            log::warn(
                "Saved level lookup for mapped practice level {} returned mismatched level {}",
                practiceLevelId,
                resolvedLevelId
            );
        }

        if (auto mainLevel = manager->getMainLevel(practiceLevelId, false)) {
            auto resolvedLevelId = mainLevel->m_levelID.value();
            if (resolvedLevelId == practiceLevelId) {
                this->switchToMappedLevel(mainLevel);
                return;
            }

            log::warn(
                "Main level lookup for mapped practice level {} returned mismatched level {}",
                practiceLevelId,
                resolvedLevelId
            );
        }

        log::warn("Could not resolve mapped practice level {} through GameLevelManager", practiceLevelId);
        showPopup(
            "Practice Mapper",
            fmt::format("Mapped practice level #{} could not be found.", practiceLevelId)
        );
    }

    void switchToMappedLevel(GJGameLevel* level) {
        auto scene = LevelInfoLayer::scene(level, false);
        if (!scene) {
            showNotice("Failed to open practice level", NotificationIcon::Warning);
            return;
        }

        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.25f, scene));
    }
};
