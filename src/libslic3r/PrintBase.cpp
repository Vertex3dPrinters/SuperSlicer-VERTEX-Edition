#include "Exception.hpp"
#include "PrintBase.hpp"

#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>

#include <regex>

#include "I18N.hpp"

//! macro used to mark string used at localization, 
//! return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r
{

size_t PrintStateBase::g_last_timestamp = 0;

// Update "scale", "input_filename", "input_filename_base" placeholders from the current m_objects.
void PrintBase::update_object_placeholders(DynamicConfig &config, const std::string &default_ext) const
{
    // get the first input file name
    std::string input_file;
    std::vector<std::string> v_scale;
	for (const ModelObject *model_object : m_model.objects) {
		ModelInstance *printable = nullptr;
		for (ModelInstance *model_instance : model_object->instances)
			if (model_instance->is_printable()) {
				printable = model_instance;
				break;
			}
		if (printable) {
	        // CHECK_ME -> Is the following correct ?
			v_scale.push_back("x:" + boost::lexical_cast<std::string>(printable->get_scaling_factor(X) * 100) +
				"% y:" + boost::lexical_cast<std::string>(printable->get_scaling_factor(Y) * 100) +
				"% z:" + boost::lexical_cast<std::string>(printable->get_scaling_factor(Z) * 100) + "%");
	        if (input_file.empty())
	            input_file = model_object->name.empty() ? model_object->input_file : model_object->name;
	    }
    }
    
    config.set_key_value("scale", new ConfigOptionStrings(v_scale));
    if (! input_file.empty()) {
        // get basename with and without suffix
        const std::string input_filename = boost::filesystem::path(input_file).filename().string();
        const std::string input_filename_base = input_filename.substr(0, input_filename.find_last_of("."));
        config.set_key_value("input_filename", new ConfigOptionString(input_filename_base + default_ext));
        config.set_key_value("input_filename_base", new ConfigOptionString(input_filename_base));
    }
}

// Generate an output file name based on the format template, default extension, and template parameters
// (timestamps, object placeholders derived from the model, current placeholder prameters, print statistics - config_override)
std::string PrintBase::output_filename(const std::string &format, const std::string &default_ext, const std::string &filename_base, const DynamicConfig *config_override) const
{
    DynamicConfig cfg;
    if (config_override != nullptr)
    	cfg = *config_override;
    PlaceholderParser::update_timestamp(cfg);
    this->update_object_placeholders(cfg, default_ext);
    if (! filename_base.empty()) {
		cfg.set_key_value("input_filename", new ConfigOptionString(filename_base + default_ext));
		cfg.set_key_value("input_filename_base", new ConfigOptionString(filename_base));
    }
    try {
        boost::filesystem::path filepath = format.empty() ?
			cfg.opt_string("input_filename_base") + default_ext :
			this->placeholder_parser().process(format, 0, &cfg);
        //remove unwanted characters
        std::string forbidden_base;
        if (const ConfigOptionString* opt = this->placeholder_parser().external_config()->option<ConfigOptionString>("gcode_filename_illegal_char")) {
            forbidden_base = opt->value;
        }
        if (!forbidden_base.empty()) {
            const std::string filename_init = filepath.stem().string();
            std::string filename = filename_init;
            std::string extension = filepath.extension().string();
            //remove {print_time} and things like that that may be computed after, and re-put them inside it after the replace.
            std::regex placehoder = std::regex("\\{[a-z_]+\\}");
            std::smatch matches;
            std::string::const_iterator searchStart(filename_init.cbegin());
            while (std::regex_search(searchStart, filename_init.cend(), matches, placehoder)) {
                for (int i = 0; i < matches.size(); i++) {
                    filename.replace(matches.position(i), matches.length(i), matches.length(i), '_');
                }
                searchStart = matches.suffix().first;
            }
            //remove unwanted characters from the cleaned string
            bool regexp_used = false;
            if (forbidden_base.front() == '(' || forbidden_base.front() == '[') {
                try {
                    filename = std::regex_replace(filename, std::regex(forbidden_base), "_");
                    regexp_used = true;
                }catch(std::exception){}
            }
            if (!regexp_used) {
                for(int i=0; i< forbidden_base.size(); i++)
                    std::replace(filename.begin(), filename.end(), forbidden_base.at(i), '_');
            }
            //re-put {print_time} and things like that
            searchStart = (filename_init.cbegin());
            while (std::regex_search(searchStart, filename_init.cend(), matches, placehoder)) {
                for (int i = 0; i < matches.size(); i++) {
                    filename.replace(matches.position(i), matches.length(i), matches.str());
                }
                searchStart = matches.suffix().first;
            }
            // set the path var
            filepath = filename + extension;
        }
        if (filepath.extension().empty())
            filepath = boost::filesystem::change_extension(filepath, default_ext);
        return filepath.string();
    } catch (std::runtime_error &err) {
        throw Slic3r::PlaceholderParserError(L("Failed processing of the output_filename_format template.") + "\n" + err.what());
    }
}

std::string PrintBase::output_filepath(const std::string &path, const std::string &filename_base) const
{
    // if we were supplied no path, generate an automatic one based on our first object's input file
    if (path.empty())
        // get the first input file name
        return (boost::filesystem::path(m_model.propose_export_file_name_and_path()).parent_path() / this->output_filename(filename_base)).make_preferred().string();
    
    // if we were supplied a directory, use it and append our automatically generated filename
    boost::filesystem::path p(path);
    if (boost::filesystem::is_directory(p))
        return (p / this->output_filename(filename_base)).make_preferred().string();
    
    // if we were supplied a file which is not a directory, use it
    return path;
}

void PrintBase::status_update_warnings(ObjectID object_id, int step, PrintStateBase::WarningLevel /* warning_level */, const std::string &message)
{
    if (this->m_status_callback)
        m_status_callback(SlicingStatus(*this, step));
    else if (! message.empty())
    	printf("%s warning: %s\n", (object_id == this->id()) ? "print" : "print object", message.c_str());
}

std::mutex& PrintObjectBase::state_mutex(PrintBase *print)
{ 
	return print->state_mutex();
}

std::function<void()> PrintObjectBase::cancel_callback(PrintBase *print)
{ 
	return print->cancel_callback();
}

void PrintObjectBase::status_update_warnings(PrintBase *print, int step, PrintStateBase::WarningLevel warning_level, const std::string &message)
{
	print->status_update_warnings(this->id(), step, warning_level, message);
}

} // namespace Slic3r
