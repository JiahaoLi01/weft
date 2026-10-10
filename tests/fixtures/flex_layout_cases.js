globalThis.caseElephantFlexFixture = {
  "fixture": "caseelephant-flex-v1",
  "tolerance_css_px": 0.025,
  "cases": [
    {
      "id": "web-row-default",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "300px",
          "height": "60px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "width": "40px",
              "height": "20px"
            }
          },
          {
            "id": "b",
            "style": {
              "width": "80px",
              "height": "30px"
            }
          }
        ]
      }
    },
    {
      "id": "column-stretch",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "column",
          "width": "300px",
          "height": "120px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "height": "20px"
            }
          },
          {
            "id": "b",
            "style": {
              "height": "40px"
            }
          }
        ]
      }
    },
    {
      "id": "grow-zero-basis",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "300px",
          "height": "40px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "flex": "1 1 0px",
              "min-width": "0"
            }
          },
          {
            "id": "b",
            "style": {
              "flex": "2 1 0px",
              "min-width": "0"
            }
          }
        ]
      }
    },
    {
      "id": "weighted-shrink",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "300px",
          "height": "40px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "width": "200px",
              "flex-shrink": "1",
              "min-width": "0"
            }
          },
          {
            "id": "b",
            "style": {
              "width": "200px",
              "flex-shrink": "2",
              "min-width": "0"
            }
          }
        ]
      }
    },
    {
      "id": "text-auto-minimum",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "180px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "word",
            "style": {
              "flex": "1"
            },
            "text": "XXXXXXXX"
          },
          {
            "id": "fixed",
            "style": {
              "width": "80px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "text-explicit-min-zero",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "180px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "word",
            "style": {
              "flex": "1",
              "min-width": "0"
            },
            "text": "XXXXXXXX"
          },
          {
            "id": "fixed",
            "style": {
              "width": "80px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "text-hidden-min-zero",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "180px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "word",
            "style": {
              "flex": "1",
              "overflow": "hidden"
            },
            "text": "XXXXXXXX"
          },
          {
            "id": "fixed",
            "style": {
              "width": "80px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "auto-min-specified-cap",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "120px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "word",
            "style": {
              "width": "60px"
            },
            "text": "XXXXXXXX"
          },
          {
            "id": "fixed",
            "style": {
              "width": "80px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "auto-min-max-cap",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "120px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "word",
            "style": {
              "flex": "1",
              "max-width": "60px"
            },
            "text": "XXXXXXXX"
          },
          {
            "id": "fixed",
            "style": {
              "width": "80px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "basis-percent-definite",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "320px",
          "height": "40px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "flex": "0 0 25%"
            }
          },
          {
            "id": "b",
            "style": {
              "flex": "0 0 50%"
            }
          }
        ]
      }
    },
    {
      "id": "basis-zero-indefinite-column",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "column",
          "width": "160px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "text",
            "style": {
              "flex": "1 1 0px",
              "min-height": "0"
            },
            "text": "XX XX XX"
          },
          {
            "id": "tail",
            "style": {
              "height": "20px",
              "width": "20px"
            }
          }
        ]
      }
    },
    {
      "id": "basis-percent-indefinite-column",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "column",
          "width": "160px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "text",
            "style": {
              "flex": "1 1 0%",
              "min-height": "0"
            },
            "text": "XX XX XX"
          },
          {
            "id": "tail",
            "style": {
              "height": "20px",
              "width": "20px"
            }
          }
        ]
      }
    },
    {
      "id": "content-box-padding-border",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "300px",
          "height": "100px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "width": "80px",
              "height": "30px",
              "padding": "10px",
              "border-width": "5px",
              "flex-shrink": "0"
            }
          },
          {
            "id": "b",
            "style": {
              "width": "50px",
              "height": "20px"
            }
          }
        ]
      }
    },
    {
      "id": "border-box-padding-border",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "300px",
          "height": "100px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "box-sizing": "border-box",
              "width": "80px",
              "height": "60px",
              "padding": "10px",
              "border-width": "5px",
              "flex-shrink": "0"
            }
          },
          {
            "id": "b",
            "style": {
              "width": "50px",
              "height": "20px"
            }
          }
        ]
      }
    },
    {
      "id": "wrap-and-gaps",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "220px",
          "height": "120px",
          "flex-wrap": "wrap",
          "gap": "10px",
          "align-content": "flex-start"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "width": "100px",
              "height": "20px",
              "flex-shrink": "0"
            }
          },
          {
            "id": "b",
            "style": {
              "width": "100px",
              "height": "30px",
              "flex-shrink": "0"
            }
          },
          {
            "id": "c",
            "style": {
              "width": "100px",
              "height": "40px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "baseline-text-font-sizes",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "300px",
          "align-items": "baseline"
        },
        "children": [
          {
            "id": "small",
            "text": "XX"
          },
          {
            "id": "large",
            "style": {
              "font-size": "40px",
              "line-height": "40px"
            },
            "text": "X"
          }
        ]
      }
    },
    {
      "id": "fixed-text-final-wrap",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "300px",
          "height": "100px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "text",
            "style": {
              "width": "100px",
              "height": "80px",
              "flex-shrink": "0"
            },
            "text": "XX XX XX"
          }
        ]
      }
    },
    {
      "id": "nested-flex-auto-min",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "160px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "inner",
            "style": {
              "display": "flex",
              "flex": "1"
            },
            "children": [
              {
                "id": "word",
                "text": "XXXXXX"
              }
            ]
          },
          {
            "id": "tail",
            "style": {
              "width": "80px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "nested-flex-min-zero",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "160px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "inner",
            "style": {
              "display": "flex",
              "flex": "1",
              "min-width": "0"
            },
            "children": [
              {
                "id": "word",
                "style": {
                  "min-width": "0"
                },
                "text": "XXXXXX"
              }
            ]
          },
          {
            "id": "tail",
            "style": {
              "width": "80px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "absolute-static-containing-block",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "position": "relative",
          "width": "300px",
          "height": "150px",
          "padding": "10px",
          "border-width": "5px"
        },
        "children": [
          {
            "id": "middle",
            "style": {
              "display": "flex",
              "width": "100px",
              "height": "60px",
              "margin-left": "20px"
            },
            "children": [
              {
                "id": "abs",
                "style": {
                  "position": "absolute",
                  "right": "10px",
                  "top": "15px",
                  "width": "40px",
                  "height": "20px"
                }
              }
            ]
          }
        ]
      }
    },
    {
      "id": "relative-containing-block",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "position": "relative",
          "width": "300px",
          "height": "150px",
          "padding": "10px",
          "border-width": "5px"
        },
        "children": [
          {
            "id": "middle",
            "style": {
              "display": "flex",
              "position": "relative",
              "width": "100px",
              "height": "60px",
              "margin-left": "20px"
            },
            "children": [
              {
                "id": "abs",
                "style": {
                  "position": "absolute",
                  "right": "10px",
                  "top": "15px",
                  "width": "40px",
                  "height": "20px"
                }
              }
            ]
          }
        ]
      }
    },
    {
      "id": "percent-inset-padding-box",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "position": "relative",
          "width": "200px",
          "height": "100px",
          "padding": "10px",
          "border-width": "5px"
        },
        "children": [
          {
            "id": "abs",
            "style": {
              "position": "absolute",
              "left": "50%",
              "top": "50%",
              "width": "20px",
              "height": "20px"
            }
          }
        ]
      }
    },
    {
      "id": "width-percent-padding-content",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "200px",
          "height": "60px",
          "padding": "10px",
          "border-width": "5px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "width": "50%",
              "height": "20px",
              "flex-shrink": "0"
            }
          },
          {
            "id": "b",
            "style": {
              "width": "25%",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "reverse-order",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "row-reverse",
          "width": "300px",
          "height": "60px",
          "gap": "10px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "width": "40px",
              "height": "20px"
            }
          },
          {
            "id": "b",
            "style": {
              "width": "80px",
              "height": "30px"
            }
          }
        ]
      }
    },
    {
      "id": "basis-points-indefinite-column",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "column",
          "width": "160px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "text",
            "style": {
              "flex": "1 1 40px",
              "min-height": "0"
            },
            "text": "XX XX XX"
          },
          {
            "id": "tail",
            "style": {
              "height": "20px",
              "width": "20px"
            }
          }
        ]
      }
    },
    {
      "id": "basis-percent40-indefinite-column",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "column",
          "width": "160px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "text",
            "style": {
              "flex": "1 1 40%",
              "min-height": "0"
            },
            "text": "XX XX XX"
          },
          {
            "id": "tail",
            "style": {
              "height": "20px",
              "width": "20px"
            }
          }
        ]
      }
    },
    {
      "id": "basis-zero-indefinite-column-padding",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "column",
          "width": "160px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "text",
            "style": {
              "flex": "1 1 0px",
              "min-height": "0",
              "padding": "10px",
              "border-width": "5px"
            },
            "text": "XX XX XX"
          },
          {
            "id": "tail",
            "style": {
              "height": "20px",
              "width": "20px"
            }
          }
        ]
      }
    },
    {
      "id": "basis-zero-indefinite-inline-row",
      "root": {
        "id": "root",
        "style": {
          "display": "inline-flex",
          "height": "40px"
        },
        "children": [
          {
            "id": "text",
            "style": {
              "flex": "1 1 0px",
              "min-width": "0"
            },
            "text": "XX"
          },
          {
            "id": "tail",
            "style": {
              "width": "20px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "column-auto-min-wrapped",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "column",
          "width": "100px",
          "height": "30px"
        },
        "children": [
          {
            "id": "text",
            "text": "XX XX XX"
          }
        ]
      }
    },
    {
      "id": "column-explicit-min-zero-wrapped",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "column",
          "width": "100px",
          "height": "30px"
        },
        "children": [
          {
            "id": "text",
            "text": "XX XX XX",
            "style": {
              "min-height": "0"
            }
          }
        ]
      }
    },
    {
      "id": "column-shrink-fixed-empty",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "flex-direction": "column",
          "width": "300px",
          "height": "20px"
        },
        "children": [
          {
            "id": "a",
            "style": {
              "height": "20px"
            }
          },
          {
            "id": "b",
            "style": {
              "height": "40px"
            }
          }
        ]
      }
    },
    {
      "id": "auto-min-max-content-box-edges",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "120px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "word",
            "style": {
              "flex": "1",
              "max-width": "60px",
              "padding": "10px",
              "border-width": "2px"
            },
            "text": "XXXXXXXX"
          },
          {
            "id": "fixed",
            "style": {
              "width": "80px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    },
    {
      "id": "auto-min-max-border-box-edges",
      "root": {
        "id": "root",
        "style": {
          "display": "flex",
          "width": "120px",
          "align-items": "flex-start"
        },
        "children": [
          {
            "id": "word",
            "style": {
              "flex": "1",
              "max-width": "60px",
              "padding": "10px",
              "border-width": "2px",
              "box-sizing": "border-box"
            },
            "text": "XXXXXXXX"
          },
          {
            "id": "fixed",
            "style": {
              "width": "80px",
              "height": "20px",
              "flex-shrink": "0"
            }
          }
        ]
      }
    }
  ],
  "input_sha256": "0aae0404653b000f5fe0e42415e33266d82ab37ef4292b28a9d168357305a681"
};
