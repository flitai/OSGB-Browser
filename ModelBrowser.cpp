#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/LineWidth>
#include <osg/Node>
#include <osgText/Text>
#include <osgText/Font>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgViewer/Viewer>
#include <osgViewer/GraphicsWindow>
#include <osgGA/TrackballManipulator>
#include <osgGA/GUIEventHandler>

#include <cstdio>
#include <cstdlib>
#include <climits>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

#ifdef __APPLE__
#include <objc/objc.h>
#include <objc/message.h>
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

// ======================= 配置 =======================
static const float WINDOW_W    = 1280.0f;
static const float WINDOW_H    = 800.0f;

// 右侧面板：标题栏 + 当前模型信息 + 文件列表 + 浏览按钮。
// 左侧显示区纯渲染，不放任何 UI
static const float LIST_W      = 270.0f;
static const float LIST_X      = WINDOW_W - LIST_W;
static const float LIST_HDR_H  = 32.0f;
static const float INFO_H      = 48.0f;
static const float LIST_ITEM_H = 22.0f;

// 面板底部按钮区：下排 Previous/Next，上排 Open Folder
static const float BTN_W       = 120.0f;
static const float BTN_H       = 44.0f;
static const float BTN_Y       = 10.0f;
static const float BTN_PREV_X  = LIST_X + 8.0f;
static const float BTN_NEXT_X  = LIST_X + 8.0f + BTN_W + 12.0f;
static const float OPEN_BTN_X  = LIST_X + 8.0f;
static const float OPEN_BTN_Y  = BTN_Y + BTN_H + 10.0f;
static const float OPEN_BTN_W  = LIST_W - 16.0f;
static const float OPEN_BTN_H  = 40.0f;
static const float BTN_AREA_H  = OPEN_BTN_Y + OPEN_BTN_H + 10.0f;

// ======================= 工具函数 =======================
static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

static bool hasWantedExt(const fs::path& p) {
    return toLower(p.extension().string()) == ".osgb";
}

// macOS Retina：帧缓冲是物理像素，而 OSG 的 Cocoa 窗口按逻辑像素上报尺寸，
// 设置视口时需要乘以屏幕缩放系数
static float displayScale() {
#ifdef __APPLE__
    id screen = ((id(*)(Class, SEL))objc_msgSend)(
        objc_getClass("NSScreen"), sel_registerName("mainScreen"));
    if (screen) {
        double s = ((double(*)(id, SEL))objc_msgSend)(
            screen, sel_registerName("backingScaleFactor"));
        if (s > 0.0) return static_cast<float>(s);
    }
#endif
    return 1.0f;
}

// 内置默认字体偏粗糙，优先加载系统字体（经 freetype 插件）
static osgText::Font* uiFont() {
    static osg::ref_ptr<osgText::Font> font = []() -> osg::ref_ptr<osgText::Font> {
        const char* candidates[] = {
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/System/Library/Fonts/Helvetica.ttc",
            "/System/Library/Fonts/Monaco.ttf",
        };
        for (const char* p : candidates) {
            if (!fs::exists(p)) continue;
            osg::ref_ptr<osgText::Font> f = osgText::readRefFontFile(p);
            if (f) return f;
        }
        std::cerr << "Warning: no usable UI font found, text may be invisible.\n";
        return nullptr;
    }();
    return font.get();
}

static void applyTextStyle(osgText::Text* t) {
    if (osgText::Font* f = uiFont()) t->setFont(f);
}

static void setWindowName(osgViewer::Viewer& viewer, const std::string& name) {
    osg::GraphicsContext* gc = viewer.getCamera()->getGraphicsContext();
    if (!gc) return;
    auto* gw = dynamic_cast<osgViewer::GraphicsWindow*>(gc);
    if (gw) gw->setWindowName(name);
}

static std::vector<fs::path> collectModelFiles(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return files;

    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return files;
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (it->is_regular_file(fec) && !fec && hasWantedExt(it->path()))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(),
              [](const fs::path& a, const fs::path& b){
                  return a.filename().string() < b.filename().string();
              });
    return files;
}

// 截断文件名，超出 maxLen 时加 "..."
static std::string truncateName(const std::string& s, size_t maxLen = 28) {
    return s.size() <= maxLen ? s : s.substr(0, maxLen - 3) + "...";
}

// 弹出 macOS 原生文件夹选择框（经 osascript），取消或失败返回空串
static std::string pickFolderDialog(const fs::path& defaultDir) {
#ifdef __APPLE__
    auto asQuote = [](const std::string& s) {
        std::string r;
        for (char c : s) { if (c == '"' || c == '\\') r += '\\'; r += c; }
        return r;
    };
    auto shQuote = [](const std::string& s) {
        std::string r = "'";
        for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
        return r + "'";
    };

    std::string script = "activate\ntry\nset f to choose folder with prompt \"Choose OSGB model folder\"";
    std::error_code ec;
    if (fs::is_directory(defaultDir, ec)) {
        script += " default location (POSIX file \"" +
                  asQuote(fs::absolute(defaultDir, ec).string()) + "\")";
    }
    script += "\nreturn POSIX path of f\non error\nreturn \"\"\nend try";

    std::string cmd = "osascript -e " + shQuote(script) + " 2>/dev/null";
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#else
    (void)defaultDir;
    return {};
#endif
}

// 创建一个带混合的矩形 Geode
static osg::ref_ptr<osg::Geode> makeRect(
    float x, float y, float w, float h, const osg::Vec4& col)
{
    osg::ref_ptr<osg::Geode> geode = new osg::Geode();
    osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();

    osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
    verts->push_back(osg::Vec3(x,     y,     0));
    verts->push_back(osg::Vec3(x + w, y,     0));
    verts->push_back(osg::Vec3(x + w, y + h, 0));
    verts->push_back(osg::Vec3(x,     y + h, 0));
    geom->setVertexArray(verts);

    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array();
    colors->push_back(col);
    geom->setColorArray(colors, osg::Array::BIND_OVERALL);
    geom->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));

    osg::StateSet* ss = geom->getOrCreateStateSet();
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setAttribute(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    geode->addDrawable(geom);
    return geode;
}

// 创建 HUD 按钮 Geode（背景 + 边框 + 文字）
static osg::ref_ptr<osg::Geode> createButton(
    float x, float y, float w, float h,
    const std::string& label, const osg::Vec4& color)
{
    osg::ref_ptr<osg::Geode> geode = new osg::Geode();

    // 背景
    osg::ref_ptr<osg::Geometry> rect = new osg::Geometry();
    osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
    verts->push_back(osg::Vec3(x,     y,     0));
    verts->push_back(osg::Vec3(x + w, y,     0));
    verts->push_back(osg::Vec3(x + w, y + h, 0));
    verts->push_back(osg::Vec3(x,     y + h, 0));
    rect->setVertexArray(verts);
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array();
    colors->push_back(color);
    rect->setColorArray(colors, osg::Array::BIND_OVERALL);
    rect->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));
    geode->addDrawable(rect);

    // 边框
    osg::ref_ptr<osg::Geometry> border = new osg::Geometry();
    border->setVertexArray(verts);
    osg::ref_ptr<osg::Vec4Array> bc = new osg::Vec4Array();
    bc->push_back(osg::Vec4(1, 1, 1, 1));
    border->setColorArray(bc, osg::Array::BIND_OVERALL);
    border->addPrimitiveSet(new osg::DrawArrays(GL_LINE_LOOP, 0, 4));
    osg::StateSet* ss = geode->getOrCreateStateSet();
    ss->setAttributeAndModes(new osg::LineWidth(2.0f), osg::StateAttribute::ON);
    geode->addDrawable(border);

    // 文字
    osg::ref_ptr<osgText::Text> text = new osgText::Text();
    applyTextStyle(text);
    text->setCharacterSize(22.0f);
    text->setAxisAlignment(osgText::TextBase::XY_PLANE);
    text->setAlignment(osgText::TextBase::CENTER_CENTER);
    text->setPosition(osg::Vec3(x + w * 0.5f, y + h * 0.5f, 0.5f));
    text->setColor(osg::Vec4(1, 1, 1, 1));
    text->setText(label);
    geode->addDrawable(text);

    return geode;
}

// 创建正交 HUD 相机
static osg::ref_ptr<osg::Camera> createHUDCamera() {
    osg::ref_ptr<osg::Camera> cam = new osg::Camera();
    cam->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
    cam->setClearMask(GL_DEPTH_BUFFER_BIT);
    cam->setRenderOrder(osg::Camera::POST_RENDER);
    cam->setAllowEventFocus(false);
    cam->setProjectionMatrix(osg::Matrix::ortho2D(0, WINDOW_W, 0, WINDOW_H));
    cam->setViewMatrix(osg::Matrix::identity());
    // 关闭自动近/远平面计算，否则近平面会被收缩到色块所在的 z=0，
    // z>0 的文字整体被裁掉
    cam->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    osg::StateSet* ss = cam->getOrCreateStateSet();
    ss->setMode(GL_LIGHTING,  osg::StateAttribute::OFF);
    ss->setMode(GL_BLEND,     osg::StateAttribute::ON);
    ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);  // 2D HUD 不需要深度测试
    ss->setAttribute(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    return cam;
}

// ======================= 浏览器核心 =======================
class ModelBrowser {
public:
    explicit ModelBrowser(const fs::path& dir)
        : files_(collectModelFiles(dir)), currentIndex_(0) {}

    bool   empty() const { return files_.empty(); }
    size_t size()  const { return files_.size(); }
    size_t index() const { return currentIndex_; }

    const fs::path& currentFile() const { return files_.at(currentIndex_); }
    const fs::path& fileAt(size_t idx)  const { return files_.at(idx); }

    osg::ref_ptr<osg::Node> loadCurrent() const {
        if (files_.empty()) return nullptr;
        return osgDB::readNodeFile(files_[currentIndex_].string());
    }

    void next()              { if (!files_.empty()) currentIndex_ = (currentIndex_ + 1) % files_.size(); }
    void prev()              { if (!files_.empty()) currentIndex_ = (currentIndex_ + files_.size() - 1) % files_.size(); }
    void jumpTo(size_t idx)  { if (idx < files_.size()) currentIndex_ = idx; }

private:
    std::vector<fs::path> files_;
    size_t currentIndex_;
};

// ======================= 应用主体 =======================
class BrowserApp {
public:
    BrowserApp(osgViewer::Viewer& viewer, const fs::path& dir)
        : viewer_(viewer), browser_(dir), currentDir_(dir)
    {
        root_       = new osg::Group();
        modelGroup_ = new osg::Group();
        root_->addChild(modelGroup_);

        // HUD：全部浏览交互集中在右侧面板
        osg::ref_ptr<osg::Camera> hud = createHUDCamera();
        listPanel_ = new osg::Group();
        hud->addChild(listPanel_);

        root_->addChild(hud);
        viewer_.setSceneData(root_);
    }

    bool init() {
        loadDirectory(currentDir_);
        return !browser_.empty();
    }

    // 切换浏览目录：重新扫描并加载第一个模型
    void loadDirectory(const fs::path& dir) {
        currentDir_       = dir;
        browser_          = ModelBrowser(dir);
        listScrollOffset_ = 0;
        status_.clear();
        modelGroup_->removeChildren(0, modelGroup_->getNumChildren());

        if (browser_.empty()) {
            std::cerr << "No model files found in: " << dir.string() << "\n";
            status_ = "No .osgb files found";
            setWindowName(viewer_, dirLabel() + " - no .osgb files");
            rebuildListPanel();
            return;
        }
        loadCurrentModel();
    }

    // 弹出文件夹选择框并切换
    void openFolder() {
        std::string picked = pickFolderDialog(currentDir_);
        if (!picked.empty()) loadDirectory(picked);
    }

    bool loadCurrentModel() {
        if (browser_.empty()) return false;
        modelGroup_->removeChildren(0, modelGroup_->getNumChildren());

        osg::ref_ptr<osg::Node> node = browser_.loadCurrent();
        if (!node) {
            status_ = "Load FAILED";
            std::string msg = "Failed to load: " + browser_.currentFile().filename().string();
            std::cerr << msg << "\n";
            setWindowName(viewer_, msg);
            ensureCurrentVisible();
            rebuildListPanel();
            return false;
        }

        status_.clear();
        modelGroup_->addChild(node);

        std::string title =
            "[" + std::to_string(browser_.index() + 1) + "/" +
            std::to_string(browser_.size()) + "] " +
            browser_.currentFile().filename().string() +
            "  -  " + dirLabel();
        setWindowName(viewer_, title);

        ensureCurrentVisible();
        rebuildListPanel();
        viewer_.home();
        return true;
    }

    void next() { browser_.next(); loadCurrentModel(); }
    void prev() { browser_.prev(); loadCurrentModel(); }

    void jumpToIndex(int idx) {
        if (idx < 0 || idx >= (int)browser_.size()) return;
        browser_.jumpTo((size_t)idx);
        loadCurrentModel();
    }

    void scrollList(int delta) {
        int maxOff = std::max(0, (int)browser_.size() - visibleItemCount());
        listScrollOffset_ = std::max(0, std::min(listScrollOffset_ + delta, maxOff));
        rebuildListPanel();
    }

    void toggleList() {
        listVisible_ = !listVisible_;
        listPanel_->setNodeMask(listVisible_ ? ~0u : 0u);
    }

    bool isListVisible()    const { return listVisible_; }
    int  listScrollOffset() const { return listScrollOffset_; }

    // 按逻辑像素尺寸设置主相机视口（内部换算为物理像素）
    void setScaledViewport(int w, int h) {
        const float s = displayScale();
        viewer_.getCamera()->setViewport(new osg::Viewport(
            0, 0, static_cast<int>(w * s), static_cast<int>(h * s)));
    }

    // 列表中可见条目数（供点击命中判断使用）
    int visibleItemCount() const {
        return static_cast<int>(
            (WINDOW_H - LIST_HDR_H - INFO_H - BTN_AREA_H) / LIST_ITEM_H);
    }

private:
    // 目录显示名：取"父目录/目录"两级，便于区分同名目录
    std::string dirLabel() const {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(currentDir_, ec);
        if (ec) p = currentDir_;
        std::string name   = p.filename().string();
        std::string parent = p.parent_path().filename().string();
        if (name.empty())   return p.string();
        if (parent.empty()) return name;
        return parent + "/" + name;
    }

    // 保证当前选中项在可视范围内
    void ensureCurrentVisible() {
        int vis = visibleItemCount();
        int idx = (int)browser_.index();
        if (idx < listScrollOffset_)
            listScrollOffset_ = idx;
        else if (idx >= listScrollOffset_ + vis)
            listScrollOffset_ = idx - vis + 1;
    }

    // 面板文字统一入口
    void addPanelText(const std::string& s, float x, float y, float size,
                      const osg::Vec4& col) {
        osg::ref_ptr<osg::Geode> g = new osg::Geode();
        osg::ref_ptr<osgText::Text> t = new osgText::Text();
        applyTextStyle(t);
        t->setCharacterSize(size);
        t->setAlignment(osgText::TextBase::LEFT_CENTER);
        t->setPosition(osg::Vec3(x, y, 0.5f));
        t->setColor(col);
        t->setText(s);
        g->addDrawable(t);
        listPanel_->addChild(g);
    }

    // 面板底部按钮：Open Folder 在上，Previous / Next 在下
    void addButtons() {
        listPanel_->addChild(createButton(OPEN_BTN_X, OPEN_BTN_Y, OPEN_BTN_W, OPEN_BTN_H,
                                          "Open Folder... (O)",
                                          osg::Vec4(0.45f, 0.30f, 0.65f, 0.85f)));
        listPanel_->addChild(createButton(BTN_PREV_X, BTN_Y, BTN_W, BTN_H, "Previous",
                                          osg::Vec4(0.15f, 0.35f, 0.75f, 0.85f)));
        listPanel_->addChild(createButton(BTN_NEXT_X, BTN_Y, BTN_W, BTN_H, "Next",
                                          osg::Vec4(0.15f, 0.55f, 0.25f, 0.85f)));
    }

    // 重建右侧面板：标题栏 + 当前模型信息 + 文件列表 + 浏览按钮
    void rebuildListPanel() {
        listPanel_->removeChildren(0, listPanel_->getNumChildren());

        // 整体背景
        listPanel_->addChild(makeRect(LIST_X, 0, LIST_W, WINDOW_H,
                                      osg::Vec4(0.07f, 0.07f, 0.12f, 0.88f)));

        // 标题栏
        listPanel_->addChild(makeRect(LIST_X, WINDOW_H - LIST_HDR_H, LIST_W, LIST_HDR_H,
                                      osg::Vec4(0.15f, 0.25f, 0.50f, 0.95f)));
        addPanelText("Files (" + std::to_string(browser_.size()) + ")   [L: show/hide]",
                     LIST_X + 8, WINDOW_H - LIST_HDR_H * 0.5f, 14.0f,
                     osg::Vec4(1, 1, 1, 1));

        // 当前模型信息区
        const float infoTop = WINDOW_H - LIST_HDR_H;
        listPanel_->addChild(makeRect(LIST_X, infoTop - INFO_H, LIST_W, INFO_H,
                                      osg::Vec4(0.10f, 0.10f, 0.18f, 0.92f)));

        if (browser_.empty()) {
            addPanelText(status_.empty() ? "No files" : status_,
                         LIST_X + 8, infoTop - INFO_H * 0.5f, 14.0f,
                         osg::Vec4(1, 0.4f, 0.4f, 1));
            addButtons();
            return;
        }

        std::string line1 = "[" + std::to_string(browser_.index() + 1) + "/" +
                            std::to_string(browser_.size()) + "]";
        osg::Vec4 line1Col(1, 1, 0.7f, 1);
        if (!status_.empty()) {
            line1 += "  " + status_;
            line1Col.set(1, 0.4f, 0.4f, 1);
        }
        addPanelText(line1, LIST_X + 8, infoTop - 14, 14.0f, line1Col);
        addPanelText(truncateName(browser_.currentFile().filename().string(), 32),
                     LIST_X + 8, infoTop - 34, 14.0f, osg::Vec4(0.9f, 0.9f, 0.9f, 1));

        // 文件列表
        int vis      = visibleItemCount();
        int total    = (int)browser_.size();
        int selected = (int)browser_.index();
        const float itemAreaTop = WINDOW_H - LIST_HDR_H - INFO_H;

        for (int i = 0; i < vis; i++) {
            int idx = listScrollOffset_ + i;
            if (idx >= total) break;

            float yBot = itemAreaTop - (i + 1) * LIST_ITEM_H;
            float yTop = yBot + LIST_ITEM_H;
            bool  isSel = (idx == selected);
            bool  isAlt = (i % 2 == 1);

            // 条目背景
            if (isSel) {
                listPanel_->addChild(makeRect(LIST_X, yBot, LIST_W, LIST_ITEM_H,
                                              osg::Vec4(0.20f, 0.45f, 0.85f, 0.92f)));
            } else if (isAlt) {
                listPanel_->addChild(makeRect(LIST_X, yBot, LIST_W, LIST_ITEM_H,
                                              osg::Vec4(0.12f, 0.12f, 0.20f, 0.55f)));
            }

            addPanelText(std::to_string(idx + 1) + ". " +
                             truncateName(browser_.fileAt(idx).filename().string()),
                         LIST_X + 6, (yTop + yBot) * 0.5f, 13.0f,
                         isSel ? osg::Vec4(1, 1, 0.85f, 1)
                               : osg::Vec4(0.80f, 0.80f, 0.80f, 1));
        }

        // 简易滚动条
        if (total > vis) {
            float barH   = itemAreaTop - BTN_AREA_H;
            float thumbH = barH * vis / (float)total;
            float thumbY = BTN_AREA_H +
                           barH * (1.0f - (float)(listScrollOffset_ + vis) / total);
            listPanel_->addChild(makeRect(WINDOW_W - 5, thumbY, 5, thumbH,
                                          osg::Vec4(0.55f, 0.55f, 0.80f, 0.75f)));
        }

        addButtons();
    }

    osgViewer::Viewer&      viewer_;
    ModelBrowser            browser_;
    osg::ref_ptr<osg::Group> root_;
    osg::ref_ptr<osg::Group> modelGroup_;
    osg::ref_ptr<osg::Group> listPanel_;
    fs::path                 currentDir_;
    std::string              status_;
    int                      listScrollOffset_ = 0;
    bool                     listVisible_      = true;
};

// ======================= 事件处理 =======================
class BrowserEventHandler : public osgGA::GUIEventHandler {
public:
    explicit BrowserEventHandler(BrowserApp& app) : app_(app) {}

    bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) override {
        switch (ea.getEventType()) {

        case osgGA::GUIEventAdapter::KEYDOWN:
            if (ea.getKey() == osgGA::GUIEventAdapter::KEY_Right ||
                ea.getKey() == 'd' || ea.getKey() == 'D') {
                app_.next(); return true;
            }
            if (ea.getKey() == osgGA::GUIEventAdapter::KEY_Left ||
                ea.getKey() == 'a' || ea.getKey() == 'A') {
                app_.prev(); return true;
            }
            if (ea.getKey() == 'l' || ea.getKey() == 'L') {
                app_.toggleList(); return true;
            }
            if (ea.getKey() == 'o' || ea.getKey() == 'O') {
                app_.openFolder(); return true;
            }
            break;

        case osgGA::GUIEventAdapter::PUSH: {
            float x = ea.getX(), y = ea.getY();

            // 面板区点击：按钮优先，其余落在列表条目上则跳转
            if (app_.isListVisible() && x >= LIST_X) {
                if (inside(x, y, BTN_PREV_X, BTN_Y, BTN_W, BTN_H)) { app_.prev(); return true; }
                if (inside(x, y, BTN_NEXT_X, BTN_Y, BTN_W, BTN_H)) { app_.next(); return true; }
                if (inside(x, y, OPEN_BTN_X, OPEN_BTN_Y, OPEN_BTN_W, OPEN_BTN_H)) {
                    app_.openFolder(); return true;
                }

                float itemAreaTop = WINDOW_H - LIST_HDR_H - INFO_H;
                if (y <= itemAreaTop && y > BTN_AREA_H) {
                    int i = (int)((itemAreaTop - y) / LIST_ITEM_H);
                    if (i >= 0 && i < app_.visibleItemCount())
                        app_.jumpToIndex(app_.listScrollOffset() + i);
                }
                return true; // 消费事件，不传给 Trackball
            }
            break;
        }

        case osgGA::GUIEventAdapter::RESIZE:
            // 窗口尺寸变化后 osgViewer 会按逻辑像素重设视口，这里再换算回物理像素
            app_.setScaledViewport(ea.getWindowWidth(), ea.getWindowHeight());
            return false;

        case osgGA::GUIEventAdapter::SCROLL:
            // 鼠标在面板区时滚动列表，否则交给 Trackball 缩放。
            // macOS 触控板/鼠标走 SCROLL_2D（连续 deltaY），滚轮走 SCROLL_UP/DOWN
            if (app_.isListVisible() && ea.getX() >= LIST_X) {
                switch (ea.getScrollingMotion()) {
                case osgGA::GUIEventAdapter::SCROLL_DOWN:
                    app_.scrollList(3);
                    break;
                case osgGA::GUIEventAdapter::SCROLL_UP:
                    app_.scrollList(-3);
                    break;
                case osgGA::GUIEventAdapter::SCROLL_2D: {
                    scrollAccum_ += ea.getScrollingDeltaY();
                    int steps = (int)scrollAccum_;
                    if (steps != 0) {
                        scrollAccum_ -= steps;
                        app_.scrollList(-steps);
                    }
                    break;
                }
                default:
                    break;
                }
                return true;
            }
            break;

        default:
            break;
        }
        return false;
    }

private:
    static bool inside(float x, float y, float rx, float ry, float rw, float rh) {
        return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
    }
    BrowserApp& app_;
    float       scrollAccum_ = 0.0f;
};

// 若以 .app 包运行，把包内 Contents/Resources 加入 osgDB 插件搜索路径
//（osgPlugins-x.x.x 目录放于其中），使打包后的程序不依赖系统安装的 OSG
static void addBundlePluginPath() {
#ifdef __APPLE__
    char buf[PATH_MAX];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) != 0) return;
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(fs::path(buf), ec);
    if (ec) exe = buf;
    fs::path res = exe.parent_path().parent_path() / "Resources";
    if (fs::is_directory(res, ec) && !ec)
        osgDB::Registry::instance()->getLibraryFilePathList()
            .push_front(res.string());
#endif
}

// ======================= 入口 =======================
int main(int argc, char** argv) {
    addBundlePluginPath();

    fs::path dir;
    if (argc > 1 && argv[1][0] != '-') {
        dir = argv[1];
    } else {
        dir = ".";
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        // Finder 双击启动时工作目录是 /，改用主目录更合理
        if (!ec && cwd == "/") {
            if (const char* home = std::getenv("HOME")) dir = home;
        }
    }

    osgViewer::Viewer viewer;
    viewer.setUpViewInWindow(100, 100,
                             static_cast<int>(WINDOW_W),
                             static_cast<int>(WINDOW_H));
    viewer.setCameraManipulator(new osgGA::TrackballManipulator());

    BrowserApp app(viewer, dir);
    app.init();

    viewer.addEventHandler(new BrowserEventHandler(app));
    viewer.setLightingMode(osg::View::HEADLIGHT);

    viewer.realize();
    app.setScaledViewport(static_cast<int>(WINDOW_W), static_cast<int>(WINDOW_H));

    return viewer.run();
}
