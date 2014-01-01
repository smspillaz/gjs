/*
 * Copyright (c) 2014 Endless Mobile, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

function removeShebangsAndParse(script, options) {
    if (script.indexOf('#!') === 0) {
        let nextNewlineIndex = script.indexOf('\n');
        script = script.slice(nextNewlineIndex);
    }

    return Reflect.parse(script, options);
}

function getSubNodesForNode(node) {
    let subNodes = [];
    switch (node.type) {
    /* These statements have a single body */
    case 'LabelledStatement':
    case 'WithStatement':
    case 'LetStatement':
    case 'ForInStatement':
    case 'ForOfStatement':
    case 'FunctionDeclaration':
    case 'FunctionExpression':
    case 'ArrowExpression':
    case 'CatchClause':
        return [node.body];
    case 'WhileStatement':
    case 'DoWhileStatement':
        return [node.body, node.test];
    case 'ForStatement':
        if (node.init != null)
            subNodes.push(node.init);
        if (node.test != null)
            subNodes.push(node.test);
        if (node.update != null)
            subNodes.push(node.update);

        subNodes.push(node.body);
        return subNodes;
    case 'BlockStatement':
        return node.body;
    case 'ThrowStatement':
    case 'ReturnStatement':
        if (node.argument != null)
            subNodes.push(node.argument);
        return subNodes;
    case 'ExpressionStatement':
        return [node.expression];
    case 'ObjectExpression':
        node.properties.forEach(function(prop) {
            subNodes.push(prop.value);
        });
        return subNodes;
    /* It is very possible that there might be something
     * interesting in the function arguments, so we need to
     * walk them too */
    case 'NewExpression':
    case 'CallExpression':
        subNodes = subNodes.concat(node.arguments);
        subNodes.push(node.callee);

        return subNodes;
    /* These statements might have multiple different bodies
     * depending on whether or not they were entered */
    case 'IfStatement':
        subNodes = [node.test, node.consequent];
        if (node.alternate != null)
            subNodes.push(node.alternate);
        return subNodes;
    case 'TryStatement':
        subNodes = [node.block];
        if (node.handler !== null)
            subNodes.push(node.handler);
        if (node.finalizer !== null)
            subNodes.push(node.finalizer);
        return subNodes;
    case 'SwitchStatement':
        subNodes = [];
        for (let i = 0; i < node.cases.length; i++) {
            let caseClause = node.cases[i];

            caseClause.consequent.forEach(function(expression) {
                subNodes.push(expression);
            });
        }

        return subNodes;
    /* Variable declarations might be initialized to
     * some expression, so traverse the tree and see if
     * we can get into the expression */
    case 'VariableDeclaration':
        node.declarations.forEach(function (declarator) {
            if (declarator.init != null) {
                subNodes.push(declarator.init);
            }
        });

        return subNodes;
    }

    return [];
}

function collectForSubNodes(subNodes, collector) {
    let collection = [];
    if (subNodes !== undefined &&
        subNodes.length > 0) {
        subNodes.forEach(function(expression) {
            let subCollection = collector(expression);
            if (subCollection !== undefined) {
                collection = collection.concat(subCollection);
            }
        });
    }

    return collection;
}

function functionNamesForNode(node) {
    let functionNames = [];
    switch (node.type) {
    case 'FunctionDeclaration':
    case 'FunctionExpression':
        if (node.id !== null) {
            functionNames.push(node.id.name);
        }
        /* If the function wasn't found, we just push a name
         * that looks like 'function:lineno' to signify that
         * this was an anonymous function. If the coverage tool
         * enters a function with no name (but a line number)
         * then it can probably use this information to
         * figure out which function it was */
        else {
            functionNames.push('function:' + node.loc.start.line);
        }
    }

    /* Recursively discover function names too */
    let subNodes = getSubNodesForNode(node);
    functionNames = functionNames.concat(collectForSubNodes(subNodes,
                                                            functionNamesForNode));

    return functionNames;
}

function functionNamesForAST(ast) {
    return collectForSubNodes(ast.body, functionNamesForNode);
}

function createBranchInfo(branchPoint, branchAlternates) {
    return {
        point: branchPoint,
        alternates: branchAlternates
    }
}

/* If a branch' consequent is a block statement, there's
 * a chance that it could start on the same line, although
 * that's not where execution really starts. If it is
 * a block statement then handle the case and go
 * to the first line where execution starts */
function getBranchExecutionStartLine(executionNode) {

    switch (executionNode.type) {
    case 'BlockStatement':
        /* Hit a block statement, but nothing inside, can never
         * be executed, tell the upper level to move on to the next
         * statement */
        if (executionNode.body.length == 0)
            return -1;

        /* Handle the case where we have nested block statements
         * that never actually get to executable code by handling
         * all statements within a block */
        for (let statement of executionNode.body) {
            let startLine = getBranchExecutionStartLine(statement);
            if (startLine != -1)
                return startLine;
        }

        /* Couldn't find an executable line inside this block */
        return -1;

    case 'SwitchCase':
        /* Hit a switch, but nothing inside, can never
         * be executed, tell the upper level to move on to the next
         * statement */
        if (executionNode.consequent.length == 0)
            return -1;

        /* Handle the case where we have nested block statements
         * that never actually get to executable code by handling
         * all statements within a block */
        for (let statement of executionNode.consequent) {
            let startLine = getBranchExecutionStartLine(statement);
            if (startLine != -1)
                return startLine;
        }

        /* Couldn't find an executable line inside this block */
        return -1;
    /* These types of statements are never executable */
    case 'EmptyStatement':
    case 'LabelledStatement':
        return -1;
    default:
        break;
    }

    return executionNode.loc.start.line;
}

function insertExecutionStartLineIfExecutable(alternates, expression) {
    let line = getBranchExecutionStartLine(expression);
    if (line !== -1)
        alternates.push(line);
}

function branchesForNode(node) {
    let branches = [];

    let alternatesForThisBranch = [];
    switch(node.type) {
    case 'IfStatement':
        insertExecutionStartLineIfExecutable(alternatesForThisBranch, node.consequent)
        if (node.alternate !== null)
            insertExecutionStartLineIfExecutable(alternatesForThisBranch, node.alternate);
        break;
    case 'WhileStatement':
    case 'DoWhileStatement':
        insertExecutionStartLineIfExecutable(alternatesForThisBranch, node.body);
        break;
    case 'SwitchStatement':

        /* The case clauses by themselves are never executable
         * so find the actual alternates */
        for (let i = 0; i < node.cases.length; i++) {
            insertExecutionStartLineIfExecutable(alternatesForThisBranch, node.cases[i]);
        }
    }

    /* Branch must have at least one alternate */
    if (alternatesForThisBranch.length) {
        branches.push(createBranchInfo(node.loc.start.line,
                                       alternatesForThisBranch))
    }

    /* Recursively discover function names too */
    let subNodes = getSubNodesForNode(node);
    branches = branches.concat(collectForSubNodes(subNodes,
                                                  branchesForNode));

    return branches;
}

function branchesForAST(ast) {
    return collectForSubNodes(ast.body, branchesForNode);
}

function executableExpressionLinesForNode(statement) {
    let allExecutableExpressionLineNos = [];

    /* Only got an undefined statement */
    if (statement === undefined)
        return undefined;

    if (statement.type.indexOf('Expression') !== -1 ||
        statement.type.indexOf('Declaration') !== -1 ||
        statement.type.indexOf('Statement') !== -1 ||
        statement.type.indexOf('Clause') !== -1 ||
        statement.type.indexOf('Literal') !== -1 ||
        statement.type.indexOf('Identifier') !== -1) {

        /* These expressions aren't executable on their own */
        switch (statement.type) {
        case 'FunctionDeclaration':
        case 'LiteralExpression':
        case 'BlockStatement':
            break;
        default:
            allExecutableExpressionLineNos.push(statement.loc.start.line);
            break;
        }
    }

    let subNodes = getSubNodesForNode(statement);
    allExecutableExpressionLineNos = allExecutableExpressionLineNos.concat(collectForSubNodes(subNodes,
                                                                                              executableExpressionLinesForNode));

    return allExecutableExpressionLineNos;
}

function executableExpressionLinesForAST(ast) {
    let allExpressions = collectForSubNodes(ast.body, executableExpressionLinesForNode);
    /* Deduplicate the list */
    allExpressions = allExpressions.filter(function(elem, pos, self) {
        return self.indexOf(elem) == pos;
    });

    return allExpressions;
}
