#include <Geode/Bindings.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/cocos.hpp>
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

        for (auto child : node->getChildrenExt()) {
            collectInteractiveBounds(child, relativeTo, ignore, out);
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
        async::TaskHolder<web::WebResponse> mappingRequest;
        int mappedPracticeLevelId = 0;
        bool requestInFlight = false;
    };

    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) {
            return false;
        }

        this->startMappingLookup(false);
        return true;
    }

    void startMappingLookup(bool showErrors) {
        if (!m_level || m_fields->requestInFlight) {
            return;
        }

        auto originalLevelId = m_level->m_levelID.value();
        if (originalLevelId <= 0) {
            return;
        }

        m_fields->requestInFlight = true;
        this->setPracticeButtonEnabled(false);

        auto url = fmt::format("{}/mapping?level_id={}", normalizedApiBaseUrl(), originalLevelId);
        web::WebRequest request;
        request.timeout(std::chrono::seconds(5));
        m_fields->mappingRequest.spawn(
            "Practice mapping lookup",
            request.get(url),
            [this, showErrors](web::WebResponse response) {
                this->onMappingResponse(std::move(response), showErrors);
            }
        );
    }

    void addPracticeButton() {
        if (!m_playBtnMenu || !m_level || m_fields->practiceButton) {
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

        if (m_fields->mappedPracticeLevelId <= 0) {
            return;
        }

        this->setPracticeButtonEnabled(false);
        this->openPracticeLevel(m_fields->mappedPracticeLevelId);
        this->setPracticeButtonEnabled(true);
    }

    void onMappingResponse(web::WebResponse response, bool showErrors) {
        if (response.cancelled()) {
            m_fields->requestInFlight = false;
            this->setPracticeButtonEnabled(true);
            return;
        }

        m_fields->requestInFlight = false;
        this->setPracticeButtonEnabled(true);

        if (!response.ok()) {
            log::warn("Practice mapping lookup failed with code {}: {}", response.code(), response.errorMessage());
            if (showErrors) {
                showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            }
            return;
        }

        auto jsonResult = response.json();
        if (!jsonResult) {
            log::warn("Practice mapping lookup returned invalid JSON: {}", jsonResult.unwrapErr());
            if (showErrors) {
                showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            }
            return;
        }

        auto payload = jsonResult.unwrap();
        if (!payload.isObject() || !payload.contains("found") || !payload["found"].isBool()) {
            log::warn("Practice mapping lookup returned an unexpected payload: {}", payload.dump());
            if (showErrors) {
                showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            }
            return;
        }

        if (!payload["found"].asBool().unwrapOr(false)) {
            return;
        }

        if (!payload.contains("practiceLevelId") || !payload["practiceLevelId"].isNumber()) {
            log::warn("Practice mapping payload is missing practiceLevelId: {}", payload.dump());
            if (showErrors) {
                showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            }
            return;
        }

        auto practiceLevelId = payload["practiceLevelId"].asInt().unwrapOr(0);
        if (practiceLevelId <= 0) {
            if (showErrors) {
                showNotice("Practice mapping lookup failed", NotificationIcon::Warning);
            }
            return;
        }

        m_fields->mappedPracticeLevelId = practiceLevelId;
        this->addPracticeButton();
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
