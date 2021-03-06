// Copyright (c) 2016, Intel Corporation.

// Testing UART APIs

console.log("UART APIs test.");

var board = require('uart');
var assert = require('Assert.js');

board.init({ port: "value", baud: 115200 }).then(function () {
    assert.true(false, "uart: set port as value");
}).catch(function (error) {
    assert.true(typeof error === "object" && error !== null,
                "uart: catch error for UART.init");
    assert.true(true, "uart: set port as value");
});

board.init({ port: "tty1", baud: 115200 }).then(function () {
    assert.true(true, "uart: set port as tty1");
}).catch(function (error) {
    assert.true(false, "uart: set port as tty1");
});

board.init({ port: "tty0", baud: 9600 }).then(function () {
    assert.true(true, "uart: set baud as 9600");
}).catch(function (error) {
    assert.true(false, "uart: set baud as 9600");
});

var currentData = "";
var readFlag = false;
var helloFlag = true;
var worldFlag = true;
var finishFlag = false;
board.init({ port: "tty0", baud: 115200 }).then(function (uart) {
    assert.true(true, "uart: set init as tty0 and 115200");
    assert.true(uart !== null && typeof uart === "object",
                "uart: callback value for UART.init");

    uart.setReadRange(1, 16);
    uart.on("read", function (data) {
        if (data.toString("ascii") === "\n" ||
            data.toString("ascii") === "\r") {
            if (helloFlag === true) {
                console.log("please send 'hello'");
                helloFlag = false;
            }

            if (currentData === "hello") {
                if (worldFlag === true) {
                    console.log("please send 'world'");
                    worldFlag = false;
                }

                uart.setReadRange(1, 16);
            }

            if (currentData === "world" && finishFlag === false) {
                assert.true(helloFlag === false && worldFlag === false,
                            "uart: read data by 'hello world'");
                assert.true(true,
                            "uart: set readrange as 1 bytes and 16 bytes");

                readFlag = true;
            }

            if (readFlag === true) {
                assert.result();

                readFlag = false;
                finishFlag = true;
            }

            uart.write(new Buffer("Data recv: " + currentData + "\r\n"));
            currentData = "";
        } else {
            currentData += data.toString("ascii");
        }
    });

    uart.write(new Buffer("UART write test\r\n")).then(function () {
        assert.true(true, "uart: write data with string");
    }).catch(function (error) {
        assert.true(false, "uart: write data with string");
    });

    uart.write(new Buffer("write: " + 1024 + "\r\n")).then(function () {
        assert.true(true, "uart: write data with number");
    }).catch(function (error) {
        assert.true(false, "uart: write data with number");
    });

    uart.write(new Buffer("write: " + true + "\r\n")).then(function () {
        assert.true(true, "uart: write data with boolean");
    }).catch(function (error) {
        assert.true(false, "uart: write data with boolean");
    });

    uart.write("write error value test\r\n").then(function () {
    }).catch(function (error) {
        assert.true(error !== null && typeof error === "object",
                    "uart: catch error for UART.write");
    });
}).catch(function (error) {
    assert.true(false, "uart: set init as tty0 and 115200");
});
