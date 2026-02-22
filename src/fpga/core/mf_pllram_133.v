`timescale 1ns/10ps

module mf_pllram_133(
    input  wire refclk,
    input  wire rst,
    output wire outclk_0,
    output wire outclk_1,
    output wire outclk_2,
    output wire outclk_3,
    output wire outclk_4,
    output wire locked
);

    altera_pll #(
        .fractional_vco_multiplier("true"),
        .reference_clock_frequency("74.25 MHz"),
        .operation_mode("normal"),
        .number_of_clocks(5),
        .output_clock_frequency0("94.857142 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .output_clock_frequency1("94.857142 MHz"),
        .phase_shift1("7906 ps"),
        .duty_cycle1(50),
        .output_clock_frequency2("132.800000 MHz"),
        .phase_shift2("0 ps"),
        .duty_cycle2(50),
        .output_clock_frequency3("132.800000 MHz"),
        .phase_shift3("0 ps"),
        .duty_cycle3(50),
        .output_clock_frequency4("132.800000 MHz"),
        .phase_shift4("1882 ps"),
        .duty_cycle4(50),
        .pll_type("General"),
        .pll_subtype("General")
    ) altera_pll_i (
        .rst    (rst),
        .outclk ({outclk_4, outclk_3, outclk_2, outclk_1, outclk_0}),
        .locked (locked),
        .fboutclk (),
        .fbclk  (1'b0),
        .refclk (refclk)
    );

endmodule
