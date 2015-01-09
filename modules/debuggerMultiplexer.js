/*
 * Copyright (c) 2015 Endless Mobile, Inc.
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
 *
 * Authored By: Sam Spilsbury <sam@endlessm.com>
 */

const Lang = imports.lang;

const __debugger = new Debugger(debuggee);

function _checkHasProperties(object, properties) {
    for (let prop of properties) {
        if (!object.hasOwnProperty(prop))
            throw new Error("Assertion failure: required property " + required);
    }
}

function _StopInfo(infoProperties) {
    _checkHasProperties(infoProperties, ['what', 'type', 'url', 'line', 'func']);
    Lang.copyProperties(infoProperties, this);
}

function _createStopInfoForFrame(what, type, frame) {
    let name = frame.callee !== null ? (frame.callee.name ? frame.callee.name :
                                                            "(anonymous)") :
                                       "(toplevel)";

    return new _StopInfo({
        what: what,
        type: type,
        url: frame.script ? frame.script.url : "(unknown)",
        line: frame.script ? frame.script.getOffsetLine(frame.offset) : 0,
        func: name
    })
}

function _createEnum() {
    const argumentsLen = arguments.length;
    let enumObject = {};
    let index = 0;

    while (index !== arguments.length) {
        enumObject[arguments[index]] = index;
        index++;
    }

    return enumObject;
}

function _appendUnique(array, element) {
    if (array.indexOf(element) === -1) {
        array.push(element);
    }
}

const DebuggerEventTypes = _createEnum('PROGRAM_STARTED',
                                       'FRAME_ENTERED',
                                       'SINGLE_STEP');

const DebuggerCommandState = _createEnum('RETURN_CONTROL',
                                         'MORE_INPUT',
                                         'NOT_PROCESSED');

function DebuggerCommandController(onStop, interactiveStart) {

    /* Some matchers. If a command satisfies the matcher property
     * then recurse into the value properties or apply th
     * remaining arguments to the function */
    const Exactly = function(node, array) {
        if (array.length > 0)
            return array[0] === node;
        return false;
    };

    const NoneRemaining = function(node, array) {
        return array.length === 0;
    };

    const callUserFunctionUntilTrue = function(userFunction) {
        let result = false;
        while (!result) {
            result = userFunction.apply(this,
                                        Array.prototype.slice.call(arguments, 1));
        }
    }

    const withDebuggerDisabled = function(dbg, handler) {
        return function() {
            /* Disable debugger inside the handler */
            dbg.enabled = false;
            let ret = handler.apply(this, arguments);
            dbg.enabled = true;
            return ret;
        }
    }

    /* Handlers for various debugger actions */
    const onFrameEntered = withDebuggerDisabled(__debugger, function(frame) {
        let stopInfo = _createStopInfoForFrame('Frame entered',
                                               DebuggerEventTypes.FRAME_ENTERED,
                                               frame);
        callUserFunctionUntilTrue(onStop, stopInfo);
        return undefined;
    });

    const onSingleStep = withDebuggerDisabled(__debugger, function() {
        /* 'this' inside the onSingleStep handler is the frame itself. */
        let stopInfo = _createStopInfoForFrame('Single step',
                                               DebuggerEventTypes.SINGLE_STEP,
                                               this);
        callUserFunctionUntilTrue(onStop, stopInfo);
    });

    /* A map of commands to syntax tree / function. This is traversed
     * in process(). Each property name in a tree corresponds to a
     * matcher name defined in matchers. If, upon calling the function
     * specified by that name, the result is true, then continue to
     * traverse the tree. If the value is an object, then it is
     * traversed as a sub-tree with the front of the array popped
     * off. If it is a function, then the function is applied to
     * the array */
    const commands = {
        step: {
            match: Exactly,
            tree: {
                frame: {
                    match: Exactly,
                    tree: {
                        _: {
                            match: NoneRemaining,
                            func: function(dbg) {
                                dbg.onEnterFrame = onFrameEntered;
                                return DebuggerCommandState.MORE_INPUT;
                            }
                        }
                    }
                },
                _: {
                    match: NoneRemaining,
                    func: function(dbg) {
                        /* Get the current frame and set the onStep handler
                         * for it. We'll remove the onStep handler as soon
                         * as we enter it. */
                        let currentFrame = dbg.getNewestFrame();

                        if (currentFrame) {
                            currentFrame.onStep = function() {
                                this.onStep = undefined;
                                onSingleStep.call(this);
                            }
                        }

                        /* Set __debugger.onEnterFrame as well, with a
                         * function wrapper that automatically unregisters
                         * itself and calls onStep as well (but this time
                         * with the entered frame as "this") */
                        dbg.onEnterFrame = Lang.bind(this, function(frame) {
                            dbg.onEnterFrame = undefined;
                            onSingleStep.call(frame);
                        });

                        return DebuggerCommandState.RETURN_CONTROL;
                    }
                }
            }
        },
        disable: {
            match: Exactly,
            tree: {
                step: {
                    match: Exactly,
                    tree: {
                        frame: {
                            match: Exactly,
                            tree: {
                                _: {
                                    match: NoneRemaining,
                                    func: function(dbg) {
                                        dbg.onEnterFrame = undefined;
                                        return DebuggerCommandState.MORE_INPUT;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        },
        cont: {
            match: Exactly,
            tree: {
                _: {
                    match: NoneRemaining,
                    func: function(dbg) {
                        /* Does nothing. This will cause us to return true
                         * and the debugger will just continue execution */
                        return DebuggerCommandState.RETURN_CONTROL;
                    }
                }
            }
        }
    };

    this._process = function(tree, commandArray) {
        let commandState = DebuggerCommandState.NOT_PROCESSED;

        for (let key of Object.keys(tree)) {
            if (tree[key].match(key, commandArray)) {
                let remainingCommands = commandArray.slice();
                remainingCommands.shift();
                /* There's a tree on this node, recurse into that tree */
                if (tree[key].hasOwnProperty('tree')) {
                    commandState = this._process(tree[key].tree,
                                                 remainingCommands);
                } else if (tree[key].hasOwnProperty('func')) {
                    /* Apply the function to the remaining arguments. The
                     * Debugger will be disabled so that all changes are atomic */
                    commandState = withDebuggerDisabled(__debugger, function() {
                        remainingCommands.unshift(__debugger);
                        return tree[key].func.apply(this, remainingCommands);
                    }) ();
                }
            }
        }

        return commandState;
    };

    /* For the very first frame, we intend to stop and ask the user what to do. This
     * hook gets cleared upon being reached */
    if (interactiveStart === true) {
        __debugger.onEnterFrame = function (frame) {
            __debugger.onEnterFrame = undefined;
            let stopInfo = _createStopInfoForFrame('Program started',
                                                   DebuggerEventTypes.PROGRAM_STARTED,
                                                   frame);
            callUserFunctionUntilTrue(onStop, stopInfo);
            return undefined;
        }
    }

    this._trackingFrames = [];
    this.handleInput = function(inputArray) {
        const result = this._process(commands, inputArray);
        if (result == DebuggerCommandState.NOT_PROCESSED)
            warning('Could not parse command set ' + inputArray);

        return result;
    }
}
