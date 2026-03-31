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
    constexpr auto kMappingsFailurePopupCooldown = std::chrono::seconds(20);

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
    std::string gLastMappingsFailureMessage;
    std::chrono::steady_clock::time_point gLastMappingsFailureAt {};

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

    std::string levelNameOrEmpty(GJGameLevel* level) {
        if (!level || level->m_levelName.empty()) {
            return "";
        }

        return std::string(level->m_levelName.c_str());
    }

    std::string invalidResolvedPracticeLevelReason(GJGameLevel* level, int expectedLevelId) {
        if (expectedLevelId <= 1) {
            return "invalid_expected_level_id";
        }

        if (!level) {
            return "missing_level";
        }

        auto resolvedLevelId = level->m_levelID.value();
        if (resolvedLevelId <= 1) {
            return fmt::format("invalid_resolved_level_id:{}", resolvedLevelId);
        }

        if (resolvedLevelId != expectedLevelId) {
            return fmt::format("mismatched_level_id:{}", resolvedLevelId);
        }

        if (levelNameOrEmpty(level).empty()) {
            return "empty_level_name";
        }

        return "";
    }

    bool isResolvedPracticeLevelValid(GJGameLevel* level, int expectedLevelId, std::string* reason = nullptr) {
        auto failureReason = invalidResolvedPracticeLevelReason(level, expectedLevelId);
        if (reason) {
            *reason = failureReason;
        }

        return failureReason.empty();
    }

    void showMappingsFailurePopup(std::string const& text) {
        auto now = std::chrono::steady_clock::now();
        if (text == gLastMappingsFailureMessage &&
            (now - gLastMappingsFailureAt) < kMappingsFailurePopupCooldown) {
            return;
        }

        gLastMappingsFailureMessage = text;
        gLastMappingsFailureAt = now;
        showPopup("Practice Mapper", text);
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
        int pendingPracticeDownloadLevelId = 0;
        std::string approvedMappingsSourceUrl;
        bool lookupInFlight = false;
        bool submitInFlight = false;
        bool practiceDownloadInFlight = false;
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

    int currentLevelId() const {
        return m_level ? m_level->m_levelID.value() : 0;
    }

    void logPracticeInfo(char const* phase, int practiceLevelId, std::string const& detail) const {
        log::info(
            "[PracticeMapper] phase={} original={} practice={} {}",
            phase,
            this->currentLevelId(),
            practiceLevelId,
            detail
        );
    }

    void logPracticeWarning(char const* phase, int practiceLevelId, std::string const& detail) const {
        log::warn(
            "[PracticeMapper] phase={} original={} practice={} {}",
            phase,
            this->currentLevelId(),
            practiceLevelId,
            detail
        );
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

        auto primaryLeftX = anchorRect.getMinX() - 8.f - buttonSize.width / 2.f;
        auto secondaryLeftX = primaryLeftX - (buttonSize.width + 6.f);
        auto tertiaryLeftX = secondaryLeftX - (buttonSize.width + 6.f);
        auto rightFallbackX = anchorRect.getMaxX() + 8.f + buttonSize.width / 2.f;
        auto upperY = anchorRect.getMidY() + buttonSize.height + 4.f;
        auto lowerY = anchorRect.getMidY() - buttonSize.height - 4.f;

        std::vector<CCPoint> candidates = {
            { primaryLeftX, anchorRect.getMidY() },
            { secondaryLeftX, anchorRect.getMidY() },
            { tertiaryLeftX, anchorRect.getMidY() },
            { primaryLeftX, upperY },
            { primaryLeftX, lowerY },
            { secondaryLeftX, upperY },
            { secondaryLeftX, lowerY },
            { rightFallbackX, anchorRect.getMidY() },
            { anchorRect.getMidX(), anchorRect.getMinY() - 20.f - buttonSize.height / 2.f },
            { anchorRect.getMidX(), anchorRect.getMaxY() + 8.f + buttonSize.height / 2.f },
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
        auto actionsBusy = m_fields->lookupInFlight || m_fields->submitInFlight || m_fields->practiceDownloadInFlight;
        this->setButtonState(
            m_fields->practiceButton,
            !actionsBusy && m_fields->mappedPracticeLevelId > 0
        );
        this->setButtonState(m_fields->browseButton, !m_fields->submitInFlight && !m_fields->practiceDownloadInFlight);
        this->setButtonState(m_fields->useButton, !m_fields->submitInFlight && !m_fields->practiceDownloadInFlight);
        this->setButtonState(m_fields->cancelButton, !m_fields->submitInFlight && !m_fields->practiceDownloadInFlight);
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

    void applyApprovedMappingsLookup(matjson::Value const& payload, std::string const& sourceUrl, bool fromCache) {
        auto originalLevelId = m_level ? m_level->m_levelID.value() : 0;
        if (originalLevelId <= 0) {
            this->finishMappingLookup(std::nullopt);
            return;
        }

        auto practiceLevelId = findPracticeLevelIdInMappings(payload, originalLevelId);
        if (practiceLevelId) {
            if (*practiceLevelId <= 1) {
                this->logPracticeWarning(
                    "lookup_result",
                    *practiceLevelId,
                    fmt::format(
                        "result=invalid_mapping source_url={} cache_hit={}",
                        sourceUrl,
                        fromCache ? "true" : "false"
                    )
                );
                showMappingsFailurePopup("Approved mappings contain an invalid practice level ID for this level.");
                this->finishMappingLookup(std::nullopt);
                return;
            }

            this->logPracticeInfo(
                "lookup_result",
                *practiceLevelId,
                fmt::format("result=found source_url={} cache_hit={}", sourceUrl, fromCache ? "true" : "false")
            );
            this->finishMappingLookup(practiceLevelId);
            return;
        }

        this->logPracticeInfo(
            "lookup_result",
            0,
            fmt::format("result=not_found source_url={} cache_hit={}", sourceUrl, fromCache ? "true" : "false")
        );
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
        m_fields->approvedMappingsSourceUrl.clear();

        auto mappingsUrl = normalizedApprovedMappingsUrl();
        if (mappingsUrl.empty()) {
            this->logPracticeWarning("lookup_abort", 0, "reason=missing_approved_mappings_url");
            showMappingsFailurePopup("Approved mappings URL is not configured. Check the GitHub settings in Practice Mapper.");
            this->finishMappingLookup(std::nullopt);
            return;
        }

        m_fields->approvedMappingsSourceUrl = mappingsUrl;

        if (hasFreshApprovedMappingsCache()) {
            this->logPracticeInfo(
                "lookup_start",
                0,
                fmt::format("source_url={} cache_hit=true", mappingsUrl)
            );
            this->applyApprovedMappingsLookup(*gApprovedMappingsCache.payload, mappingsUrl, true);
            return;
        }

        m_fields->lookupInFlight = true;
        this->refreshActionButtons();
        this->logPracticeInfo(
            "lookup_start",
            0,
            fmt::format("source_url={} cache_hit=false", mappingsUrl)
        );

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
            this->logPracticeWarning(
                "lookup_error",
                0,
                fmt::format(
                    "source_url={} code={} message={}",
                    m_fields->approvedMappingsSourceUrl,
                    response.code(),
                    response.errorMessage()
                )
            );
            showMappingsFailurePopup("Could not load approved mappings from GitHub. Check the repository, path, or URL settings.");
            this->finishMappingLookup(std::nullopt);
            return;
        }

        auto jsonResult = response.json();
        if (!jsonResult) {
            this->logPracticeWarning(
                "lookup_error",
                0,
                fmt::format(
                    "source_url={} reason=invalid_json detail={}",
                    m_fields->approvedMappingsSourceUrl,
                    jsonResult.unwrapErr()
                )
            );
            showMappingsFailurePopup("Approved mappings JSON is invalid. Check the GitHub mappings file.");
            this->finishMappingLookup(std::nullopt);
            return;
        }

        auto payload = jsonResult.unwrap();
        if (!payload.isObject()) {
            this->logPracticeWarning(
                "lookup_error",
                0,
                fmt::format(
                    "source_url={} reason=unexpected_payload payload={}",
                    m_fields->approvedMappingsSourceUrl,
                    payload.dump()
                )
            );
            showMappingsFailurePopup("Approved mappings JSON must be an object of original level IDs to practice level IDs.");
            this->finishMappingLookup(std::nullopt);
            return;
        }

        cacheApprovedMappings(payload);
        this->applyApprovedMappingsLookup(payload, m_fields->approvedMappingsSourceUrl, false);
    }

    void onPracticePressed(CCObject*) {
        if (
            m_fields->lookupInFlight ||
            m_fields->submitInFlight ||
            m_fields->practiceDownloadInFlight ||
            m_fields->mappedPracticeLevelId <= 0
        ) {
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

        if (practiceLevelId <= 1) {
            this->logPracticeWarning("open_abort", practiceLevelId, "reason=invalid_expected_level_id");
            showPopup(
                "Practice Mapper",
                fmt::format("Mapped practice level #{} is invalid.", practiceLevelId)
            );
            return;
        }

        this->logPracticeInfo(
            "open_start",
            practiceLevelId,
            fmt::format("source_url={}", m_fields->approvedMappingsSourceUrl)
        );

        if (auto savedLevel = manager->getSavedLevel(practiceLevelId)) {
            std::string failureReason;
            if (isResolvedPracticeLevelValid(savedLevel, practiceLevelId, &failureReason)) {
                this->logPracticeInfo(
                    "open_cache_hit",
                    practiceLevelId,
                    fmt::format(
                        "path=saved_level callback_level_id={} level_name=\"{}\"",
                        savedLevel->m_levelID.value(),
                        levelNameOrEmpty(savedLevel)
                    )
                );
                this->switchToMappedLevel(savedLevel, "saved_level");
                return;
            }

            this->logPracticeWarning(
                "open_cache_invalid",
                practiceLevelId,
                fmt::format(
                    "reason={} callback_level_id={} level_name=\"{}\"",
                    failureReason,
                    savedLevel->m_levelID.value(),
                    levelNameOrEmpty(savedLevel)
                )
            );
        }

        if (m_fields->practiceDownloadInFlight) {
            this->logPracticeWarning("open_abort", practiceLevelId, "reason=download_already_in_flight");
            return;
        }

        m_fields->practiceDownloadInFlight = true;
        m_fields->pendingPracticeDownloadLevelId = practiceLevelId;
        manager->m_levelDownloadDelegate = this;
        manager->downloadLevel(practiceLevelId, false, 0);
        this->updateActionButtonStates();
        this->logPracticeInfo("open_download_start", practiceLevelId, "path=download");
        showNotice(fmt::format("Downloading mapped practice level #{}...", practiceLevelId));
    }

    void levelDownloadFinished(GJGameLevel* level) {
        if (!m_fields->practiceDownloadInFlight || m_fields->pendingPracticeDownloadLevelId <= 0) {
            LevelInfoLayer::levelDownloadFinished(level);
            return;
        }

        auto expectedLevelId = m_fields->pendingPracticeDownloadLevelId;
        m_fields->practiceDownloadInFlight = false;
        m_fields->pendingPracticeDownloadLevelId = 0;
        this->updateActionButtonStates();

        std::string failureReason;
        if (!isResolvedPracticeLevelValid(level, expectedLevelId, &failureReason)) {
            auto callbackLevelId = level ? level->m_levelID.value() : 0;
            this->logPracticeWarning(
                "open_download_invalid",
                expectedLevelId,
                fmt::format(
                    "reason={} callback_level_id={} level_name=\"{}\"",
                    failureReason,
                    callbackLevelId,
                    levelNameOrEmpty(level)
                )
            );
            showPopup(
                "Practice Mapper",
                fmt::format("Mapped practice level #{} could not be found.", expectedLevelId)
            );
            return;
        }

        this->logPracticeInfo(
            "open_download_complete",
            expectedLevelId,
            fmt::format(
                "callback_level_id={} level_name=\"{}\"",
                level->m_levelID.value(),
                levelNameOrEmpty(level)
            )
        );
        this->switchToMappedLevel(level, "download");
    }

    void levelDownloadFailed(int response) {
        if (!m_fields->practiceDownloadInFlight || m_fields->pendingPracticeDownloadLevelId <= 0) {
            LevelInfoLayer::levelDownloadFailed(response);
            return;
        }

        auto expectedLevelId = m_fields->pendingPracticeDownloadLevelId;
        m_fields->practiceDownloadInFlight = false;
        m_fields->pendingPracticeDownloadLevelId = 0;
        this->updateActionButtonStates();

        log::warn(
            "[PracticeMapper] phase=open_download_failed original={} practice={} response={}",
            this->currentLevelId(),
            expectedLevelId,
            response
        );
        showPopup(
            "Practice Mapper",
            fmt::format("Mapped practice level #{} could not be downloaded.", expectedLevelId)
        );
    }

    void switchToMappedLevel(GJGameLevel* level, char const* path) {
        auto scene = LevelInfoLayer::scene(level, false);
        if (!scene) {
            this->logPracticeWarning(
                "open_abort",
                level ? level->m_levelID.value() : 0,
                fmt::format("reason=scene_creation_failed path={}", path)
            );
            showPopup("Practice Mapper", "Failed to open the mapped practice level.");
            return;
        }

        this->logPracticeInfo(
            "open_commit",
            level ? level->m_levelID.value() : 0,
            fmt::format("path={} level_name=\"{}\"", path, levelNameOrEmpty(level))
        );
        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.25f, scene));
    }
};
