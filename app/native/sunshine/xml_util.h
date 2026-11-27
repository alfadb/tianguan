// SPDX-License-Identifier: GPL-3.0-or-later
//
// 极简 XML 解析：只为解析 Sunshine 的 serverinfo / applist / launch / pair 响应。
// 支持元素名、属性、文本、可重复的同名子元素；不追求 DTD、命名空间等通用特性。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tg {
namespace sunshine {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;  // 直接文本内容（已解码实体、去除首尾空白）
    std::vector<XmlNode> children;

    bool has_attribute(const std::string& key) const;
    std::string attribute(const std::string& key, const std::string& fallback = "") const;

    // 直接子元素中第一个同名者；没有返回 nullptr。
    const XmlNode* child(const std::string& name) const;
    std::vector<const XmlNode*> children_named(const std::string& name) const;

    // 深度优先（先序）查找第一个同名后代，语义同 moonlight-qt NvHTTP::getXmlString。
    const XmlNode* find_first(const std::string& name) const;

    // 点分路径，如 "root.AppTitle" 或 "root.App.ID"。
    // 第一段与本节点名相同则从本节点开始，否则在第一段上做深度优先查找。
    const XmlNode* path(const std::string& dotted) const;

    // 取直接子元素文本；缺失返回 fallback。
    std::string child_text(const std::string& name, const std::string& fallback = "") const;
};

struct XmlDocument {
    bool ok = false;
    std::string error;
    XmlNode root;
};

XmlDocument parse_xml(const std::string& text);

}  // namespace sunshine
}  // namespace tg
