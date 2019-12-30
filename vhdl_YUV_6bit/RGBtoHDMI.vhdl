----------------------------------------------------------------------------------
-- Engineer:            David Banks
--
-- Create Date:         15/7/2018
-- Module Name:         RGBtoHDMI CPLD
-- Project Name:        RGBtoHDMI
-- Target Devices:      XC9572XL
--
-- Version:             1.0
--
----------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity RGBtoHDMI is
    Port (
        -- From Atom L/PA/PB Comparators
        AL_I:      in    std_logic;
        AH_I:      in    std_logic;
        BL_I:      in    std_logic;
        BH_I:      in    std_logic;
        LL_I:      in    std_logic;
        LH_I:      in    std_logic;
        HS_I:      in    std_logic;
        FS_I:      in    std_logic;

        -- To Atom L/PA/PB Comparators
        clamp:     out   std_logic;

        -- From Pi
        clk:       in    std_logic;
        sp_clk:    in    std_logic;
        sp_clken:  in    std_logic;
        sp_data:   in    std_logic;
        mux:       in    std_logic;

        -- To PI GPIO
        quad:      out   std_logic_vector(11 downto 0);
        psync:     out   std_logic;
        csync:     out   std_logic;

        -- User interface
        version:   in    std_logic
    );
end RGBtoHDMI;

architecture Behavorial of RGBtoHDMI is

    -- Version number: Design_Major_Minor
    -- Design: 0 = Normal CPLD, 1 = Alternative CPLD, 2=Atom CPLD, 3=YUV6847 CPLD
    constant VERSION_NUM  : std_logic_vector(11 downto 0) := x"357";

    -- Default offset to start sampling at
    constant default_offset   : unsigned(9 downto 0) := to_unsigned(1024 - 256 - 255 + 8, 10);

    -- Turn on back porch clamp
    constant atom_clamp_start : unsigned(9 downto 0) := to_unsigned(1024 - 255 + 48, 10);

    -- Turn off back port clamo
    constant atom_clamp_end   : unsigned(9 downto 0) := to_unsigned(1024 - 255 + 248, 10);

    -- Sampling points
    constant INIT_SAMPLING_POINTS : std_logic_vector(8 downto 0) := "000110000";

    signal shift_R : std_logic_vector(1 downto 0);
    signal shift_G : std_logic_vector(1 downto 0);
    signal shift_B : std_logic_vector(1 downto 0);
    signal shift_rl : std_logic_vector(1 downto 0);
    signal shift_gl : std_logic_vector(1 downto 0);
    signal shift_bl : std_logic_vector(1 downto 0);

    -- The sampling counter runs at 8x pixel clock of 7.15909MHz = 56.272720MHz
    --
    -- The luminance signal is sampled every  8 counts (bits 2..0)
    -- The chromance signal is sampled every 16 counts (bits 3..0)
    -- The pixel shift register is shifter every 4 counts (bits 1..0)
    --    (i.e. each pixel is replicated twice)
    -- The quad counter is bits 3..2
    -- The psync flag is bit 4
    --
    -- At the moment we don't count pixels with the line, the Pi does that
    signal counter  : unsigned(9 downto 0);

    -- Sample point register;
    signal sp_reg   : std_logic_vector(8 downto 0) := INIT_SAMPLING_POINTS;

    -- Break out of sp_reg
    signal offset   : unsigned (3 downto 0);
    signal filter_C : std_logic;
    signal filter_L : std_logic;
    signal invert   : std_logic;
    signal subsam_C : std_logic;
    signal alt_R    : std_logic;

    -- State to determine whether to invert A
    signal inv_R    : std_logic;

    -- Sample pixel on next clock; pipelined to reduce the number of product terms
    signal sample_C : std_logic;
    signal sample_L : std_logic;

    -- R/PA/PB processing pipeline
    signal AL1      : std_logic;
    signal AH1      : std_logic;
    signal BL1      : std_logic;
    signal BH1      : std_logic;
    signal LL1      : std_logic;
    signal LH1      : std_logic;

    signal AL2      : std_logic;
    signal AH2      : std_logic;
    signal BL2      : std_logic;
    signal BH2      : std_logic;
    signal LL2      : std_logic;
    signal LH2      : std_logic;

    signal AL3      : std_logic;
    signal AH3      : std_logic;
    signal BL3      : std_logic;
    signal BH3      : std_logic;
    signal LL3      : std_logic;
    signal LH3      : std_logic;

    signal AL       : std_logic;
    signal AH       : std_logic;
    signal BL       : std_logic;
    signal BH       : std_logic;
    signal LL       : std_logic;
    signal LH       : std_logic;

    signal HS1      : std_logic;
    signal HS2      : std_logic;
    signal HS3      : std_logic;

    signal LL_S      : std_logic;
    signal LH_S      : std_logic;
    signal swap_bits : std_logic;

    signal HS_counter : unsigned(1 downto 0);

begin
    offset <= unsigned(sp_reg(3 downto 0));
    filter_C <= sp_reg(4);
    filter_L <= sp_reg(5);
    invert <= sp_reg(6);
    subsam_C <= sp_reg(7);
    alt_R <= sp_reg(8);

    swap_bits <= FS_I when mux = '1' else '0';

    LL_S <= LH_I when swap_bits = '1' else LL_I;
    LH_S <= LL_I when swap_bits = '1' else LH_I;

    -- Shift the bits in LSB first
    process(sp_clk)
    begin
        if rising_edge(sp_clk) then
            if sp_clken = '1' then
                sp_reg <= sp_data & sp_reg(sp_reg'left downto sp_reg'right + 1);
            end if;
        end if;
    end process;

    process(clk)
    begin
        if rising_edge(clk) then

            -- synchronize CSYNC to the sampling clock
            HS1 <= HS_I xor invert;

            -- De-glitch HS
            --    HS1 is the possibly glitchy input
            --    HS2 is the filtered output
            if HS1 = HS2 then
                -- output same as input, reset the counter
                HS_counter <= to_unsigned(0, HS_counter'length);
            else
                -- output different to input
                HS_counter <= HS_counter + 1;
                -- if the difference lasts for N-1 cycles, update the output
                if HS_counter = 3 then
                    HS2 <= HS1;
                end if;
            end if;

            HS3 <= HS2;

            -- Counter is used to find sampling point for first pixel
            if HS3 = '1' and HS2 = '0' then
                counter <= default_offset;
                if alt_R = '1' then
                    inv_R <= not inv_R;
                else
                    inv_R <= '0';
                end if;
            elsif counter(counter'left) = '1' then
                if HS2 = '0' and "000" & counter(8 downto 0) = x"1FF" then
                    -- synchronise inv_R to frame sync pulse
                    if alt_R = '1' then
                        inv_R <= '1';
                    end if;
                else
                    counter <= counter + 1;
                end if;
            else
                counter(5 downto 0) <= counter(5 downto 0) + 1;
            end if;

            -- sample luminance signal
            if counter(2 downto 0) = (not offset(2)) & offset(1 downto 0) then
                sample_L <= '1';
            else
                sample_L <= '0';
            end if;

            -- sample colour signal
            if (subsam_C = '0' and counter(2 downto 0) = (not offset(2)) & offset(1 downto 0)) or
               (subsam_C = '1' and counter(3 downto 0) = (not offset(3)) & offset(2 downto 0)) then
                sample_C <= '1';
            else
                sample_C <= '0';
            end if;

            -- Chroma / Luma Filtering and Sampling
            AL1 <= AL_I;
            AH1 <= AH_I;
            BL1 <= BL_I;
            BH1 <= BH_I;

            AL2 <= AL1;
            AH2 <= AH1;
            BL2 <= BL1;
            BH2 <= BH1;

            LL1 <= LL_S;
            LH1 <= LH_S;
            LL2 <= LL1;
            LH2 <= LH1;

            if sample_C = '1' then
                if filter_C = '1' then
                    AL <= (AL1 AND AL2) OR (AL1 AND AL_I) OR (AL2 AND AL_I);
                    AH <= (AH1 AND AH2) OR (AH1 AND AH_I) OR (AH2 AND AH_I);
                    BL <= (BL1 AND BL2) OR (BL1 AND BL_I) OR (BL2 AND BL_I);
                    BH <= (BH1 AND BH2) OR (BH1 AND BH_I) OR (BH2 AND BH_I);
                else
                    AL <= AL1;
                    AH <= AH1;
                    BL <= BL1;
                    BH <= BH1;
                end if;
            end if;

            if sample_L = '1' then
                if filter_L = '1' then
                    LL <= (LL1 AND LL2) OR (LL1 AND LL_S) OR (LL2 AND LL_S);
                    LH <= (LH1 AND LH2) OR (LH1 AND LH_S) OR (LH2 AND LH_S);
                else
                    LL <= LL1;
                    LH <= LH1;
                end if;
            end if;

            if sample_L = '1' and counter(counter'left) = '0' then
                -- R Sample/shift register
                if inv_R = '1' and AH = AL then
                    -- invert R on alternate lines (i.e. 00->11 and 11->00)
                    shift_R <= not(AH) & shift_R(1);
                else
                    shift_R <= AH & shift_R(1);
                end if;
                -- G Sample/shift register
                shift_G <= LH & shift_G(1);
                -- B Sample/shift register
                shift_B <= BH & shift_B(1);
                 -- R Sample/shift register
                if inv_R = '1' and AH = AL then
                    -- invert R on alternate lines (i.e. 00->11 and 11->00)
                    shift_rl <= not(AL) & shift_rl(1);
                else
                    shift_rl <= AL & shift_rl(1);
                end if;
                -- G Sample/shift register
                shift_gl <= LL & shift_gl(1);
                -- B Sample/shift register
                shift_bl <= BL & shift_bl(1);
            end if;

            -- Output quad register
            if version = '0' then
                quad  <= VERSION_NUM;
                psync <= FS_I;
            elsif counter(counter'left) = '0' then
                if counter(3 downto 0) = "0000" then
                    quad(11 downto 6) <= shift_bl(1) & shift_gl(1) & shift_rl(1) & shift_B(1) & shift_G(1) & shift_R(1);
                    quad( 5 downto 0) <= shift_bl(0) & shift_gl(0) & shift_rl(0) & shift_B(0) & shift_G(0) & shift_R(0);
                end if;
                if counter(3 downto 0) = "0010" then
                    psync    <= counter(4);
                end if;
            else
                quad  <= (others => '0');
                psync <= '0';
            end if;

            -- generate the clamp output
            if counter >= atom_clamp_start AND counter < atom_clamp_end then
                clamp <= '1';
            else
                clamp <= '0';
            end if;

            -- generate the csync output
            csync <= HS2;

        end if;
    end process;

end Behavorial;
