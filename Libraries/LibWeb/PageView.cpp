/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AK/LexicalPath.h>
#include <AK/URL.h>
#include <LibCore/File.h>
#include <LibCore/MimeData.h>
#include <LibGUI/Application.h>
#include <LibGUI/Painter.h>
#include <LibGUI/ScrollBar.h>
#include <LibGUI/Window.h>
#include <LibGemini/Document.h>
#include <LibGfx/ImageDecoder.h>
#include <LibJS/Runtime/Value.h>
#include <LibMarkdown/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/HTMLAnchorElement.h>
#include <LibWeb/DOM/HTMLImageElement.h>
#include <LibWeb/DOM/MouseEvent.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Frame.h>
#include <LibWeb/Layout/LayoutDocument.h>
#include <LibWeb/Layout/LayoutNode.h>
#include <LibWeb/PageView.h>
#include <LibWeb/Parser/HTMLDocumentParser.h>
#include <LibWeb/Parser/HTMLParser.h>
#include <LibWeb/RenderingContext.h>
#include <LibWeb/ResourceLoader.h>
#include <stdio.h>

//#define SELECTION_DEBUG

namespace Web {

PageView::PageView()
    : m_main_frame(Web::Frame::create(*this))
{
    main_frame().on_set_needs_display = [this](auto& content_rect) {
        if (content_rect.is_empty()) {
            update();
            return;
        }
        Gfx::Rect adjusted_rect = content_rect;
        adjusted_rect.set_location(to_widget_position(content_rect.location()));
        update(adjusted_rect);
    };

    set_should_hide_unnecessary_scrollbars(true);
    set_background_role(ColorRole::Base);
}

PageView::~PageView()
{
}

void PageView::set_document(Document* new_document)
{
    RefPtr<Document> old_document = document();

    if (new_document == old_document)
        return;

    if (old_document)
        old_document->on_layout_updated = nullptr;

    main_frame().set_document(new_document);

    if (on_set_document)
        on_set_document(new_document);

    if (new_document) {
        new_document->on_layout_updated = [this] {
            layout_and_sync_size();
            update();
        };
    }

#ifdef HTML_DEBUG
    if (document != nullptr) {
        dbgprintf("\033[33;1mLayout tree before layout:\033[0m\n");
        ::dump_tree(*layout_root());
    }
#endif

    layout_and_sync_size();
    update();
}

void PageView::layout_and_sync_size()
{
    if (!document())
        return;

    bool had_vertical_scrollbar = vertical_scrollbar().is_visible();
    bool had_horizontal_scrollbar = horizontal_scrollbar().is_visible();

    main_frame().set_size(available_size());
    document()->layout();
    set_content_size(enclosing_int_rect(layout_root()->rect()).size());

    // NOTE: If layout caused us to gain or lose scrollbars, we have to lay out again
    //       since the scrollbars now take up some of the available space.
    if (had_vertical_scrollbar != vertical_scrollbar().is_visible() || had_horizontal_scrollbar != horizontal_scrollbar().is_visible()) {
        main_frame().set_size(available_size());
        document()->layout();
        set_content_size(enclosing_int_rect(layout_root()->rect()).size());
    }

    main_frame().set_viewport_rect(viewport_rect_in_content_coordinates());

#ifdef HTML_DEBUG
    dbgprintf("\033[33;1mLayout tree after layout:\033[0m\n");
    ::dump_tree(*layout_root());
#endif
}

void PageView::resize_event(GUI::ResizeEvent& event)
{
    GUI::ScrollableWidget::resize_event(event);
    layout_and_sync_size();
}

void PageView::paint_event(GUI::PaintEvent& event)
{
    GUI::Frame::paint_event(event);

    GUI::Painter painter(*this);
    painter.add_clip_rect(widget_inner_rect());
    painter.add_clip_rect(event.rect());

    if (!layout_root()) {
        painter.fill_rect(event.rect(), palette().color(background_role()));
        return;
    }

    painter.fill_rect(event.rect(), document()->background_color(palette()));

    if (auto background_bitmap = document()->background_image()) {
        painter.draw_tiled_bitmap(event.rect(), *background_bitmap);
    }

    painter.translate(frame_thickness(), frame_thickness());
    painter.translate(-horizontal_scrollbar().value(), -vertical_scrollbar().value());

    RenderingContext context(painter, palette());
    context.set_should_show_line_box_borders(m_should_show_line_box_borders);
    context.set_viewport_rect(viewport_rect_in_content_coordinates());
    layout_root()->render(context);
}

void PageView::mousemove_event(GUI::MouseEvent& event)
{
    if (!layout_root())
        return GUI::ScrollableWidget::mousemove_event(event);

    bool hovered_node_changed = false;
    bool is_hovering_link = false;
    bool was_hovering_link = document()->hovered_node() && document()->hovered_node()->is_link();
    auto result = layout_root()->hit_test(to_content_position(event.position()));
    const HTMLAnchorElement* hovered_link_element = nullptr;
    if (result.layout_node) {
        RefPtr<Node> node = result.layout_node->node();
        hovered_node_changed = node != document()->hovered_node();
        document()->set_hovered_node(node);
        if (node) {
            hovered_link_element = node->enclosing_link_element();
            if (hovered_link_element) {
#ifdef HTML_DEBUG
                dbg() << "PageView: hovering over a link to " << hovered_link_element->href();
#endif
                is_hovering_link = true;
            }
            auto offset = compute_mouse_event_offset(event.position(), *result.layout_node);
            node->dispatch_event(MouseEvent::create("mousemove", offset.x(), offset.y()));
        }
        if (m_in_mouse_selection) {
            layout_root()->selection().set_end({ result.layout_node, result.index_in_node });
            dump_selection("MouseMove");
            update();
        }
    }
    if (window())
        window()->set_override_cursor(is_hovering_link ? GUI::StandardCursor::Hand : GUI::StandardCursor::None);
    if (hovered_node_changed) {
        update();
        RefPtr<HTMLElement> hovered_html_element = document()->hovered_node() ? document()->hovered_node()->enclosing_html_element() : nullptr;
        if (hovered_html_element && !hovered_html_element->title().is_null()) {
            auto screen_position = screen_relative_rect().location().translated(event.position());
            GUI::Application::the().show_tooltip(hovered_html_element->title(), screen_position.translated(4, 4));
        } else {
            GUI::Application::the().hide_tooltip();
        }
    }
    if (is_hovering_link != was_hovering_link) {
        if (on_link_hover) {
            on_link_hover(hovered_link_element ? document()->complete_url(hovered_link_element->href()).to_string() : String());
        }
    }
    event.accept();
}

void PageView::mousedown_event(GUI::MouseEvent& event)
{
    if (!layout_root())
        return GUI::ScrollableWidget::mousemove_event(event);

    bool hovered_node_changed = false;
    auto result = layout_root()->hit_test(to_content_position(event.position()));
    if (result.layout_node) {
        RefPtr<Node> node = result.layout_node->node();
        hovered_node_changed = node != document()->hovered_node();
        document()->set_hovered_node(node);
        if (node) {
            auto offset = compute_mouse_event_offset(event.position(), *result.layout_node);
            node->dispatch_event(MouseEvent::create("mousedown", offset.x(), offset.y()));
            if (RefPtr<HTMLAnchorElement> link = node->enclosing_link_element()) {
                dbg() << "PageView: clicking on a link to " << link->href();

                if (event.button() == GUI::MouseButton::Left) {
                    if (link->href().starts_with("javascript:")) {
                        run_javascript_url(link->href());
                    } else {
                        if (on_link_click)
                            on_link_click(link->href(), link->target(), event.modifiers());
                    }
                } else if (event.button() == GUI::MouseButton::Right) {
                    if (on_link_context_menu_request)
                        on_link_context_menu_request(link->href(), event.position().translated(screen_relative_rect().location()));
                } else if (event.button() == GUI::MouseButton::Middle) {
                    if (on_link_middle_click)
                        on_link_middle_click(link->href());
                }
            } else {
                if (event.button() == GUI::MouseButton::Left) {
                    if (layout_root())
                        layout_root()->selection().set({ result.layout_node, result.index_in_node }, {});
                    dump_selection("MouseDown");
                    m_in_mouse_selection = true;
                }
            }
        }
    }
    if (hovered_node_changed)
        update();
    event.accept();
}

void PageView::mouseup_event(GUI::MouseEvent& event)
{
    if (!layout_root())
        return GUI::ScrollableWidget::mouseup_event(event);

    auto result = layout_root()->hit_test(to_content_position(event.position()));
    if (result.layout_node) {
        if (RefPtr<Node> node = result.layout_node->node()) {
            auto offset = compute_mouse_event_offset(event.position(), *result.layout_node);
            node->dispatch_event(MouseEvent::create("mouseup", offset.x(), offset.y()));
        }
    }

    if (event.button() == GUI::MouseButton::Left) {
        dump_selection("MouseUp");
        m_in_mouse_selection = false;
    }
}

void PageView::keydown_event(GUI::KeyEvent& event)
{
    if (event.modifiers() == 0) {
        switch (event.key()) {
        case Key_Home:
            vertical_scrollbar().set_value(0);
            break;
        case Key_End:
            vertical_scrollbar().set_value(vertical_scrollbar().max());
            break;
        case Key_Down:
            vertical_scrollbar().set_value(vertical_scrollbar().value() + vertical_scrollbar().step());
            break;
        case Key_Up:
            vertical_scrollbar().set_value(vertical_scrollbar().value() - vertical_scrollbar().step());
            break;
        case Key_Left:
            horizontal_scrollbar().set_value(horizontal_scrollbar().value() + horizontal_scrollbar().step());
            break;
        case Key_Right:
            horizontal_scrollbar().set_value(horizontal_scrollbar().value() - horizontal_scrollbar().step());
            break;
        case Key_PageDown:
            vertical_scrollbar().set_value(vertical_scrollbar().value() + frame_inner_rect().height());
            break;
        case Key_PageUp:
            vertical_scrollbar().set_value(vertical_scrollbar().value() - frame_inner_rect().height());
            break;
        default:
            break;
        }
    }

    event.accept();
}

void PageView::reload()
{
    load(main_frame().document()->url());
}

static RefPtr<Document> create_markdown_document(const ByteBuffer& data, const URL& url)
{
    auto markdown_document = Markdown::Document::parse(data);
    if (!markdown_document)
        return nullptr;

    return parse_html_document(markdown_document->render_to_html(), url);
}

static RefPtr<Document> create_text_document(const ByteBuffer& data, const URL& url)
{
    auto document = adopt(*new Document(url));

    auto html_element = document->create_element("html");
    document->append_child(html_element);

    auto head_element = document->create_element("head");
    html_element->append_child(head_element);
    auto title_element = document->create_element("title");
    head_element->append_child(title_element);

    auto title_text = document->create_text_node(url.basename());
    title_element->append_child(title_text);

    auto body_element = document->create_element("body");
    html_element->append_child(body_element);

    auto pre_element = create_element(document, "pre");
    body_element->append_child(pre_element);

    pre_element->append_child(document->create_text_node(String::copy(data)));
    return document;
}

static RefPtr<Document> create_image_document(const ByteBuffer& data, const URL& url)
{
    auto document = adopt(*new Document(url));

    auto image_decoder = Gfx::ImageDecoder::create(data.data(), data.size());
    auto bitmap = image_decoder->bitmap();
    ASSERT(bitmap);

    auto html_element = create_element(document, "html");
    document->append_child(html_element);

    auto head_element = create_element(document, "head");
    html_element->append_child(head_element);
    auto title_element = create_element(document, "title");
    head_element->append_child(title_element);

    auto basename = LexicalPath(url.path()).basename();
    auto title_text = adopt(*new Text(document, String::format("%s [%dx%d]", basename.characters(), bitmap->width(), bitmap->height())));
    title_element->append_child(title_text);

    auto body_element = create_element(document, "body");
    html_element->append_child(body_element);

    auto image_element = create_element(document, "img");
    image_element->set_attribute("src", url.to_string());
    body_element->append_child(image_element);

    return document;
}

static RefPtr<Document> create_gemini_document(const ByteBuffer& data, const URL& url)
{
    auto markdown_document = Gemini::Document::parse({ (const char*)data.data(), data.size() }, url);

    return parse_html_document(markdown_document->render_to_html(), url);
}

String encoding_from_content_type(const String& content_type)
{
    auto offset = content_type.index_of("charset=");
    if (offset.has_value())
        return content_type.substring(offset.value() + 8, content_type.length() - offset.value() - 8).to_lowercase();

    return "utf-8";
}

String mime_type_from_content_type(const String& content_type)
{
    auto offset = content_type.index_of(";");
    if (offset.has_value())
        return content_type.substring(0, offset.value()).to_lowercase();

    return content_type;
}

static String guess_mime_type_based_on_filename(const URL& url)
{
    if (url.path().ends_with(".png"))
        return "image/png";
    if (url.path().ends_with(".gif"))
        return "image/gif";
    if (url.path().ends_with(".md"))
        return "text/markdown";
    if (url.path().ends_with(".html") || url.path().ends_with(".htm"))
        return "text/html";
    return "text/plain";
}

RefPtr<Document> PageView::create_document_from_mime_type(const ByteBuffer& data, const URL& url, const String& mime_type, const String& encoding)
{
    if (mime_type.starts_with("image/"))
        return create_image_document(data, url);
    if (mime_type == "text/plain")
        return create_text_document(data, url);
    if (mime_type == "text/markdown")
        return create_markdown_document(data, url);
    if (mime_type == "text/gemini")
        return create_gemini_document(data, url);
    if (mime_type == "text/html") {
        if (m_use_old_parser)
            return parse_html_document(data, url, encoding);
        HTMLDocumentParser parser(data, encoding);
        parser.run(url);
        return parser.document();
    }
    return nullptr;
}

void PageView::load(const URL& url)
{
    dbg() << "PageView::load: " << url;

    if (!url.is_valid()) {
        load_error_page(url, "Invalid URL");
        return;
    }

    if (window())
        window()->set_override_cursor(GUI::StandardCursor::None);

    if (on_load_start)
        on_load_start(url);

    ResourceLoader::the().load(
        url,
        [this, url](auto data, auto& response_headers) {
            // FIXME: Also check HTTP status code before redirecting
            auto location = response_headers.get("Location");
            if (location.has_value()) {
                load(location.value());
                return;
            }

            if (data.is_null()) {
                load_error_page(url, "No data");
                return;
            }

            String encoding = "utf-8";
            String mime_type;

            auto content_type = response_headers.get("Content-Type");
            if (content_type.has_value()) {
                dbg() << "Content-Type header: _" << content_type.value() << "_";
                encoding = encoding_from_content_type(content_type.value());
                mime_type = mime_type_from_content_type(content_type.value());
            } else {
                dbg() << "No Content-Type header to go on! Guessing based on filename...";
                mime_type = guess_mime_type_based_on_filename(url);
            }

            dbg() << "I believe this content has MIME type '" << mime_type << "', encoding '" << encoding << "'";
            auto document = create_document_from_mime_type(data, url, mime_type, encoding);
            ASSERT(document);
            set_document(document);

            if (!url.fragment().is_empty())
                scroll_to_anchor(url.fragment());

            if (on_title_change)
                on_title_change(document->title());
        },
        [this, url](auto error) {
            load_error_page(url, error);
        });

    if (url.protocol() != "file" && url.protocol() != "about") {
        URL favicon_url;
        favicon_url.set_protocol(url.protocol());
        favicon_url.set_host(url.host());
        favicon_url.set_port(url.port());
        favicon_url.set_path("/favicon.ico");

        ResourceLoader::the().load(
            favicon_url,
            [this, favicon_url](auto data, auto&) {
                dbg() << "Favicon downloaded, " << data.size() << " bytes from " << favicon_url;
                auto decoder = Gfx::ImageDecoder::create(data.data(), data.size());
                auto bitmap = decoder->bitmap();
                if (!bitmap) {
                    dbg() << "Could not decode favicon " << favicon_url;
                    return;
                }
                dbg() << "Decoded favicon, " << bitmap->size();
                if (on_favicon_change)
                    on_favicon_change(*bitmap);
            });
    }

    this->scroll_to_top();
}

void PageView::load_error_page(const URL& failed_url, const String& error)
{
    auto error_page_url = "file:///res/html/error.html";
    ResourceLoader::the().load(
        error_page_url,
        [this, failed_url, error](auto data, auto&) {
            ASSERT(!data.is_null());
            auto html = String::format(
                String::copy(data).characters(),
                escape_html_entities(failed_url.to_string()).characters(),
                escape_html_entities(error).characters());
            auto document = parse_html_document(html, failed_url);
            ASSERT(document);
            set_document(document);
            if (on_title_change)
                on_title_change(document->title());
        },
        [](auto error) {
            dbg() << "Failed to load error page: " << error;
            ASSERT_NOT_REACHED();
        });
}

const LayoutDocument* PageView::layout_root() const
{
    return document() ? document()->layout_node() : nullptr;
}

LayoutDocument* PageView::layout_root()
{
    if (!document())
        return nullptr;
    return const_cast<LayoutDocument*>(document()->layout_node());
}

void PageView::scroll_to_anchor(const StringView& name)
{
    if (!document())
        return;

    const auto* element = document()->get_element_by_id(name);
    if (!element) {
        auto candidates = document()->get_elements_by_name(name);
        for (auto* candidate : candidates) {
            if (is<HTMLAnchorElement>(*candidate)) {
                element = to<HTMLAnchorElement>(candidate);
                break;
            }
        }
    }

    if (!element) {
        dbg() << "PageView::scroll_to_anchor(): Anchor not found: '" << name << "'";
        return;
    }
    if (!element->layout_node()) {
        dbg() << "PageView::scroll_to_anchor(): Anchor found but without layout node: '" << name << "'";
        return;
    }
    auto& layout_node = *element->layout_node();
    Gfx::FloatRect float_rect { layout_node.box_type_agnostic_position(), { (float)visible_content_rect().width(), (float)visible_content_rect().height() } };
    scroll_into_view(enclosing_int_rect(float_rect), true, true);
    window()->set_override_cursor(GUI::StandardCursor::None);
}

Document* PageView::document()
{
    return main_frame().document();
}

const Document* PageView::document() const
{
    return main_frame().document();
}

void PageView::dump_selection(const char* event_name)
{
    UNUSED_PARAM(event_name);
#ifdef SELECTION_DEBUG
    dbg() << event_name << " selection start: "
          << layout_root()->selection().start().layout_node << ":" << layout_root()->selection().start().index_in_node << ", end: "
          << layout_root()->selection().end().layout_node << ":" << layout_root()->selection().end().index_in_node;
#endif
}

void PageView::did_scroll()
{
    main_frame().set_viewport_rect(viewport_rect_in_content_coordinates());
    main_frame().did_scroll({});
}

Gfx::Point PageView::compute_mouse_event_offset(const Gfx::Point& event_position, const LayoutNode& layout_node) const
{
    auto content_event_position = to_content_position(event_position);
    auto top_left_of_layout_node = layout_node.box_type_agnostic_position();

    return {
        content_event_position.x() - static_cast<int>(top_left_of_layout_node.x()),
        content_event_position.y() - static_cast<int>(top_left_of_layout_node.y())
    };
}

void PageView::run_javascript_url(const String& url)
{
    ASSERT(url.starts_with("javascript:"));
    if (!document())
        return;

    auto source = url.substring_view(11, url.length() - 11);
    dbg() << "running js from url: _" << source << "_";
    document()->run_javascript(source);
}

void PageView::drop_event(GUI::DropEvent& event)
{
    if (event.mime_data().has_urls()) {
        if (on_url_drop) {
            on_url_drop(event.mime_data().urls().first());
            return;
        }
    }
    ScrollableWidget::drop_event(event);
}

}
