/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "gtestresult.h"
#include "gtestconstants.h"
#include "../testframeworkmanager.h"
#include "../testtreeitem.h"

#include <utils/id.h>
#include <utils/qtcassert.h>

#include <QRegularExpression>

namespace Autotest {
namespace Internal {

GTestResult::GTestResult(const QString &id, const QString &projectFile,
                         const QString &name)
    : TestResult(id, name), m_projectFile(projectFile)
{
}

const QString GTestResult::outputString(bool selected) const
{
    const QString &desc = description();
    QString output;
    switch (result()) {
    case ResultType::Pass:
    case ResultType::Fail:
        output = m_testCaseName;
        if (selected && !desc.isEmpty())
            output.append('\n').append(desc);
        break;
    default:
        output = desc;
        if (!selected)
            output = output.split('\n').first();
    }
    return output;
}

bool GTestResult::isDirectParentOf(const TestResult *other, bool *needsIntermediate) const
{
    if (!TestResult::isDirectParentOf(other, needsIntermediate))
        return false;

    const GTestResult *gtOther = static_cast<const GTestResult *>(other);
    if (m_testCaseName == gtOther->m_testCaseName) {
        const ResultType otherResult = other->result();
        if (otherResult == ResultType::MessageInternal || otherResult == ResultType::MessageLocation)
            return result() != ResultType::MessageInternal && result() != ResultType::MessageLocation;
    }
    if (m_iteration != gtOther->m_iteration)
        return false;
    return isTestSuite() && gtOther->isTestCase();
}

static QString normalizeName(const QString &name)
{
    static QRegularExpression parameterIndex("/\\d+");

    QString nameWithoutParameterIndices = name;
    nameWithoutParameterIndices.remove(parameterIndex);

    return nameWithoutParameterIndices.split('/').last();
}

static QString normalizeTestName(const QString &testname)
{
    QString nameWithoutTypeParam = testname.split(',').first();

    return normalizeName(nameWithoutTypeParam);
}

const TestTreeItem *GTestResult::findTestTreeItem() const
{
    auto id = Utils::Id(Constants::FRAMEWORK_PREFIX).withSuffix(GTest::Constants::FRAMEWORK_NAME);
    ITestFramework *framework = TestFrameworkManager::frameworkForId(id);
    QTC_ASSERT(framework, return nullptr);
    const TestTreeItem *rootNode = framework->rootNode();
    if (!rootNode)
        return nullptr;

    const auto item = rootNode->findAnyChild([this](const Utils::TreeItem *item) {
        const auto treeItem = static_cast<const TestTreeItem *>(item);
        return treeItem && matches(treeItem);
    });
    return static_cast<const TestTreeItem *>(item);
}

bool GTestResult::matches(const TestTreeItem *treeItem) const
{
    if (treeItem->proFile() != m_projectFile)
        return false;

    if (isTestSuite())
        return matchesTestSuite(treeItem);

    return matchesTestCase(treeItem);
}

bool GTestResult::matchesTestCase(const TestTreeItem *treeItem) const
{
    if (treeItem->type() != TestTreeItem::TestCase)
        return false;

    const QString testItemTestCase = treeItem->parentItem()->name() + '.' + treeItem->name();
    return testItemTestCase == normalizeName(m_testCaseName);
}

bool GTestResult::matchesTestSuite(const TestTreeItem *treeItem) const
{
    if (treeItem->type() != TestTreeItem::TestSuite)
        return false;

    return treeItem->name() == normalizeTestName(name());
}

} // namespace Internal
} // namespace Autotest
