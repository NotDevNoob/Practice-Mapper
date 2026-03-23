#include <Geode/Bindings.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>

#include <chrono>
#include <optional>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr float kPracticeButtonScale = 0.62f;
    constexpr float kButtonGap = 12.f;
    constexpr float kScreenMargin = 18.f;

    std::string normalizedApiBaseUrl() {
        auto value = Mod::get()->getSettingValue<std::string>("api-base-url");
        while (!value.empty() && value.back() == '/') {
            value.pop_back();
        }
        return value;
    }

    void showNotice(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
        Notification::create(text, icon, 1.1f)->show();
    }

    void showPopup(std::string const& title, std::string const& text) {
        FLAlertLayer::create(title.c_str(), text, "OK")->show();
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

        auto children = node->getChildren();
        if (!children) {
            return;
        }

        CCObject* object = nullptr;
        CCARRAY_FOREACH(children, object) {
            collectInteractiveBounds(static_cast<CCNode*>(object), relativeTo, ignore, out);
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
        EventListener<web::WebTask> mappingListener;
        int pendingPracticeLevelId = 0;
        bool requestInFlight = false;
    };

    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) {
            return false;
        }

        this->addPracticeButton();
        return true;
    }

    void addPracticeButton() {
        if (!m_playBtnMenu || !m_level) {
            return;
        }

        auto practiceSprite = CCSprite::createWithSpriteFrameName("GJ_practiceBtn_001.png");
        if (!practiceSprite) {
            log::warn("Could not load GJ_practiceBtn_001.png");
            return;
        }
        practiceSprite->setScale(kPracticeButtonScale);

        auto practiceButton = CCMenuItemSpriteExtra::create(
            practiceSprite,
            this,
            menu_selector(PracticeMappingLevelInfoLayer::onPracticePressed)
        );
        practiceButton->setID("practice-mapping-button");

        auto overlayMenu = CCMenu::create();
        overlayMenu->setID("practice-mapping-menu");
        overlayMenu->setContentSize(this->getContentSize());
        overlayMenu->ignoreAnchorPointForPosition(false);
        overlayMenu->setAnchorPoint({ 0.f, 0.f });
        overlayMenu->setPosition({ 0.f, 0.f });

        auto position = this->findSafeButtonPosition(practiceButton);
        if (!position) {
            log::warn("Unable to find safe placement for practice mapping button");
            return;
        }

        practiceButton->setPosition(*position);
        overlayMenu->addChild(practiceButton);
        this->addChild(overlayMenu, 25);

        m_fields->overlayMenu = overlayMenu;
        m_fields->practiceButton = practiceButton;
        m_fields->mappingListener.bind(this, &PracticeMappingLevelInfoLayer::onMappingResponse);
    }

    std::optional<CCPoint> findSafeButtonPosition(CCMenuItemSpriteExtra* button) {
        auto playButton = this->findPrimaryPlayButton();
        if (!playButton) {
            return std::nullopt;
        }

        auto layerSize = this->getContentSize();
        auto anchorRect = rectInNodeSpace(playButton, this);
        auto buttonSize = button->getScaledContentSize();

        std::vector<CCRect> occupied;
        collectInteractiveBounds(this, this, button, occupied);

        std::vector<CCPoint> candidates = {
            { anchorRect.getMaxX() + kButtonGap + buttonSize.width / 2.f, anchorRect.getMidY() },
            { anchorRect.getMinX() - kButtonGap - buttonSize.width / 2.f, anchorRect.getMidY() },
            { anchorRect.getMaxX() + kButtonGap + buttonSize.width / 2.f, anchorRect.getMaxY() - buttonSize.height / 2.f },
            { anchorRect.getMinX() - kButtonGap - buttonSize.width / 2.f, anchorRect.getMaxY() - buttonSize.height / 2.f },
            { anchorRect.getMidX(), anchorRect.getMaxY() + kButtonGap + buttonSize.height / 2.f },
            { anchorRect.getMidX(), anchorRect.getMinY() - kButtonGap - buttonSize.height / 2.f },
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

    CCMenuItemSpriteExtra* findPrimaryPlayButton() {
        auto children = m_playBtnMenu->getChildren();
        if (!children) {
            return nullptr;
        }

        CCMenuItemSpriteExtra* best = nullptr;
        float bestArea = 0.f;

        CCObject* object = nullptr;
        CCARRAY_FOREACH(children, object) {
            auto item = typeinfo_cast<CCMenuItemSpriteExtra*>(static_cast<CCNode*>(object));
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

    void setPracticeButtonEnabled(bool enabled) {
        if (!m_fields->practiceButton) {
            return;
        }

        m_fields->practiceButton->setEnabled(enabled);
        m_fields->practiceButton->setOpacity(enabled ? 255 : 140);
    }

    void onPracticePressed(CCObject*) {
        if (!m_level || m_fields->requestInFlight) {
            return;
        }

        auto originalLevelId = m_level->m_levelID.value();
        if (originalLevelId <= 0) {
            showNotice("No practice mapping available", NotificationIcon::Info);
            return;
        }

        m_fields->requestInFlight = true;
        this->setPracticeButtonEnabled(false);

        auto url = fmt::format("{}/mapping?level_id={}", normalizedApiBaseUrl(), originalLevelId);
        web::WebRequest request;
        request.timeout(std::chrono::seconds(5));
        m_fields->mappingListener.setFilter(request.get(url));
    }

    void onMappingResponse(web::WebTask::Event* event) {
        if (!event) {
            return;
        }

        if (event->getProgress()) {
            return;
        }

        if (event->isCancelled()) {
            m_fields->requestInFlight = false;
            this->setPracticeButtonEnabled(true);
            return;
        }

        auto response = event->getValue();
        if (!response) {
            return;
        }

        m_fields->requestInFlight = false;
        this->setPracticeButtonEnabled(true);

        if (!response->ok()) {
            log::warn("Practice mapping lookup failed with code {}: {}", response->code(), response->errorMessage());
            showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            return;
        }

        auto jsonResult = response->json();
        if (!jsonResult) {
            log::warn("Practice mapping lookup returned invalid JSON: {}", jsonResult.unwrapErr());
            showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            return;
        }

        auto payload = jsonResult.unwrap();
        if (!payload.isObject() || !payload.contains("found") || !payload["found"].isBool()) {
            log::warn("Practice mapping lookup returned an unexpected payload: {}", payload.dump());
            showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            return;
        }

        if (!payload["found"].asBool().unwrapOr(false)) {
            showNotice("No practice mapping available", NotificationIcon::Info);
            return;
        }

        if (!payload.contains("practiceLevelId") || !payload["practiceLevelId"].isNumber()) {
            log::warn("Practice mapping payload is missing practiceLevelId: {}", payload.dump());
            showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            return;
        }

        auto practiceLevelId = payload["practiceLevelId"].asInt().unwrapOr(0);
        if (practiceLevelId <= 0) {
            showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            return;
        }

        this->openPracticeLevel(practiceLevelId);
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

    void levelDownloadFinished(GJGameLevel* level) {
        if (m_fields->pendingPracticeLevelId > 0 && level && level->m_levelID.value() == m_fields->pendingPracticeLevelId) {
            m_fields->pendingPracticeLevelId = 0;
            this->switchToMappedLevel(level);
            return;
        }

        LevelInfoLayer::levelDownloadFinished(level);
    }

    void levelDownloadFailed(int response) {
        if (m_fields->pendingPracticeLevelId > 0) {
            log::warn("Failed to download mapped practice level {} (response {})", m_fields->pendingPracticeLevelId, response);
            m_fields->pendingPracticeLevelId = 0;
            showNotice("Failed to load practice level", NotificationIcon::Warning);
            return;
        }

        LevelInfoLayer::levelDownloadFailed(response);
    }
};
