class Environment:
    def __init__(self, width, height, font, icon_font):
        # Screen dimensions in pixel
        self.screen_width = width
        self.screen_height = height
        self.status_height = 0
        self.cols = 0
        self.rows = 0
        self.font = None
        self.font_name = ""
        self.icon_font = None
        self.icon_font_name = None
        self.kvm = None
        self.tui = None
        self.term = None
        self.sts = None
        self.shell = None
        self.audio = None
        self.sd_busy = False
        self.update_font(font)
        self.update_icon_font(icon_font)

    def update_font(self, font_name):
        self.font = __import__(f"fonts.{font_name}", None, None, [font_name])
        self.font_name = font_name

        # Reserve 1 row for a topbar
        self.status_height = self.font.HEIGHT

        # How many characters can we fit on the screen?
        self.cols = self.screen_width // self.font.WIDTH
        usable_height = self.screen_height - self.status_height
        self.rows = usable_height // self.font.HEIGHT

        if self.term:
            self.term.update_layout(self.font, self.cols, self.rows)
            self.term.top_offset(self.status_height)
            self.sts.update_width(self.cols)

    def update_icon_font(self, font_name):
        if font_name is None:
            self.icon_font = None
            self.icon_font_name = None
        else:
            self.icon_font = __import__(f"fonts.{font_name}", None, None, [font_name])
            self.icon_font_name = font_name

        if self.term:
            self.term.set_icon_font(self.icon_font)

